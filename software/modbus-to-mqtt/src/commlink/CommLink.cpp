#include <DNSServer.h>
#include "commlink/CommLink.h"
#include "Config.h"
#include "ESPAsyncWebServer.h"
#include <atomic>
#include "services/IndicatorService.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>

static std::atomic<bool> s_mqttEnabled{false};
static CommLink *s_activeCommLink = nullptr;
static constexpr auto MQTT_CLIENT_PREFIX = "MODBUS_CLIENT-";
static constexpr auto MQTT_TASK_STACK = 4096;
static constexpr auto MQTT_TASK_LOOP_DELAY_MS = 100;
static constexpr auto RND_SEED = 0xffff;

CommLink::CommLink(MqttSubscriptionHandler *subscriptionHandler, PubSubClient *mqttClient, Logger *logger)
    : _mqttClient(mqttClient),
      _logger(logger),
      _mqttTaskHandle(nullptr),
      _subscriptionHandler(subscriptionHandler){
    s_activeCommLink = this;
}

void handleMqttMessage(char *topic, const byte *payload, const unsigned int length) {
    if (s_activeCommLink != nullptr) {
        const auto topicStr = String(topic);
        s_activeCommLink->onMqttMessage(topicStr, payload, length);
    }
}

 auto CommLink::begin() -> bool {
    _mqttClient->setBufferSize(MQTT_BUFFER_SIZE);
    // Load broker/port/user/pass from NVS with SPIFFS fallback
    loadMQTTConfig();

    const char *broker = {LOCAL_MQTT_BROKER};
    _mqttClient->setServer(broker, atoi(LOCAL_MQTT_PORT));
    _mqttClient->setCallback(handleMqttMessage);

    // Do NOT attempt connection here; Wi‑Fi/LWIP may not be initialized yet.
    // The background task will handle connecting once Wi‑Fi is up.
    setMQTTEnabled(true);
    return startMqttTask();
}

 void CommLink::loadMQTTConfig() {
     // Read non-sensitive values from SPIFFS only
     String server = "0.0.0.0";
     String port = "1883";
     String user;
     if (SPIFFS.exists("/conf/mqtt.json")) {
         File f = SPIFFS.open("/conf/mqtt.json", FILE_READ);
         if (f) {
             String text = f.readString();
             f.close();
             JsonDocument doc;
             if (!deserializeJson(doc, text)) {
                 String ip = doc["broker_ip"] | "";
                 String url = doc["broker_url"] | "";
                 String p = doc["broker_port"] | "1883";
                 String u = doc["user"] | "";
                 auto extractHost = [](const String &uurl) {
                     if (uurl.length() == 0) return String("");
                     int start = uurl.indexOf("://");
                     start = (start >= 0) ? (start + 3) : 0;
                     int slash = uurl.indexOf('/', start);
                     int colon = uurl.indexOf(':', start);
                     int end;
                     if (slash >= 0 && colon >= 0) end = (slash < colon) ? slash : colon; else if (slash >= 0) end = slash; else if (colon >= 0) end = colon; else end = uurl.length();
                     return uurl.substring(start, end);
                 };
                 ip.trim(); url.trim(); p.trim(); u.trim();
                 if (ip.length() && ip != "0.0.0.0") server = ip; else if (url.length()) server = extractHost(url);
                 if (p.length()) port = p;
                 user = u;
             }
         }
     }
     // Apply to local buffers
     strcpy(LOCAL_MQTT_BROKER, server.c_str());
     strcpy(LOCAL_MQTT_PORT, port.c_str());
     if (user.length()) strcpy(LOCAL_MQTT_USER, user.c_str()); else LOCAL_MQTT_USER[0] = '\0';

     // Read password from NVS only
     preferences.begin(MQTT_PREFS_NAMESPACE, false);
     if (preferences.isKey("pass")) {
         strcpy(LOCAL_MQTT_PASSWORD, preferences.getString("pass").c_str());
     } else {
         LOCAL_MQTT_PASSWORD[0] = '\0';
     }
     preferences.end();
 }

auto CommLink::ensureMQTTConnection() const -> bool {
    String clientId = MQTT_CLIENT_PREFIX;
    clientId += String(random(RND_SEED), HEX);
    if (LOCAL_MQTT_BROKER[0] == '\0' || String(LOCAL_MQTT_BROKER) == "0.0.0.0") {
        _logger->logWarning("MQTT broker not configured; skipping connection attempt");
        return false;
    }
    _logger->logInformation((String("Connecting to MQTT broker [") + LOCAL_MQTT_BROKER + ":" + String(LOCAL_MQTT_PORT) + "]").c_str());
    const bool connected = _mqttClient->connect(clientId.c_str(), LOCAL_MQTT_USER, LOCAL_MQTT_PASSWORD);
    if (!connected) {
        for (int i = 0; i < 3; i++) {
            //setLedColor(true, false, false);
            delay(500);
            _logger->logError(("MQTT connect failed, rc=" + String(_mqttClient->state())).c_str());
        }
    } else {
        IndicatorService::instance().setMqttConnected(true);
    }

    for (const auto &topic: _subscriptionHandler->getHandlerTopics()) {
        _mqttClient->subscribe(topic.c_str());
        _logger->logInformation(("MQTT subscribe to: " + topic).c_str());
    }

    return connected;
}

[[noreturn]] void CommLink::processMQTTAsync(void *parameter) {
    const auto *commLink = static_cast<CommLink *>(parameter);
    constexpr TickType_t delayTicks = MQTT_TASK_LOOP_DELAY_MS / portTICK_PERIOD_MS;
    static unsigned long lastReconnectAttempt = 0;
    while (true) {

        if (!CommLink::isMQTTEnabled()) {
            vTaskDelay(delayTicks);
            continue;
        }

        // Only interact with MQTT when Wi‑Fi is connected
        if (WiFiClass::status() != WL_CONNECTED) {
            IndicatorService::instance().setMqttConnected(false);
            vTaskDelay(delayTicks);
            continue;
        }

        const bool connectedNow = commLink->_mqttClient->connected();
        IndicatorService::instance().setMqttConnected(connectedNow);
        if (!connectedNow) {
            commLink->_logger->logError("MQTT disconnected, attempting reconnect");
            const unsigned long now = millis();
            if (now - lastReconnectAttempt >= MQTT_RECONNECT_INTERVAL_MS) {
                lastReconnectAttempt = now;
                if (!commLink->ensureMQTTConnection()) {
                    commLink->_logger->logError("MQTT reconnect attempt failed in task loop");
                }
            }
        }
        commLink->_mqttClient->loop();
        vTaskDelay(delayTicks);
    }
}

bool CommLink::startMqttTask() {
    const BaseType_t result = xTaskCreatePinnedToCore(
        processMQTTAsync,
        "processMQTTAsync",
        MQTT_TASK_STACK,
        this,
        1,
        &_mqttTaskHandle,
        1
    );

    if (result != pdPASS) {
        return false;
    }
    return true;
}

bool CommLink::mqttPublish(const char *topic, const char *payload) const {
    return _mqttClient->publish(topic, payload);
}

void CommLink::onMqttMessage(const String &topic, const uint8_t *payload, const size_t length) const {
    String message;
    message.reserve(length);
    for (size_t i = 0; i < length; i++) {
        message += static_cast<char>(payload[i]);
    }
    _subscriptionHandler->handle(topic, message);
    _logger->logDebug("CommLink::onMqttMessage - Received MQTT message");
}

char *CommLink::getMqttBroker() {
    return LOCAL_MQTT_BROKER;
}

int CommLink::getMQTTState() const {
    return _mqttClient->state();
}

char *CommLink::getMQTTUser() {
    return LOCAL_MQTT_USER;
}

void CommLink::setMQTTEnabled(bool enabled) {
    s_mqttEnabled.store(enabled, std::memory_order_release);
}
bool CommLink::isMQTTEnabled() {
    return s_mqttEnabled.load(std::memory_order_acquire);
}


bool CommLink::testConnectOnce() {
    // Load settings and try a single connect, do not start task
    loadMQTTConfig();
    const char *broker = {LOCAL_MQTT_BROKER};
    _logger->logInformation((String("Test connect to MQTT [") + broker + ":" + String(LOCAL_MQTT_PORT) + "]").c_str());
    _mqttClient->setServer(broker, atoi(LOCAL_MQTT_PORT));
    if (WiFiClass::status() != WL_CONNECTED) {
        _logger->logError("MQTT test connect requested but Wi-Fi not connected");
        return false;
    }
    return ensureMQTTConnection();
}
