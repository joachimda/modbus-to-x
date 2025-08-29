#include <DNSServer.h>
#include "commlink/CommLink.h"
#include "Config.h"
#include "ESPAsyncWebServer.h"
#include <atomic>

static std::atomic<bool> s_mqttEnabled{false};

static CommLink *s_activeCommLink = nullptr;
static constexpr auto DEFAULT_MQTT_BROKER_PORT = "1883";
static constexpr auto DEFAULT_MQTT_BROKER_IP = "0.0.0.0";

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
    setupLED();
    _mqttClient->setBufferSize(MQTT_BUFFER_SIZE);

    const char *broker = {LOCAL_MQTT_BROKER};
    _logger->logInformation(
        ("Connecting to MQTT broker [" + String(broker) + ":" + String(LOCAL_MQTT_PORT) + "]").c_str());
    _mqttClient->setServer(broker, atoi(LOCAL_MQTT_PORT));
    if (!ensureMQTTConnection()) {
        _logger->logError("MQTT connection failed");
    }
    _mqttClient->setCallback(handleMqttMessage);
    return startMqttTask();
}

void CommLink::loadMQTTConfig() {
    preferences.begin(MQTT_PREFS_NAMESPACE, false);

    if (preferences.isKey("server")) {
        strcpy(LOCAL_MQTT_BROKER, preferences.getString("server").c_str());
    } else {
        strcpy(LOCAL_MQTT_BROKER, DEFAULT_MQTT_BROKER_IP);
    }

    if (preferences.isKey("port")) {
        strcpy(LOCAL_MQTT_PORT, preferences.getString("port").c_str());
    } else {
        strcpy(LOCAL_MQTT_PORT, DEFAULT_MQTT_BROKER_PORT);
    }

    if (preferences.isKey("user")) {
        strcpy(LOCAL_MQTT_USER, preferences.getString("user").c_str());
    } else {
        LOCAL_MQTT_USER[0] = '\0';
    }

    if (preferences.isKey("pass")) {
        strcpy(LOCAL_MQTT_PASSWORD, preferences.getString("pass").c_str());
    } else {
        LOCAL_MQTT_PASSWORD[0] = '\0';
    }

    preferences.end();
}

void CommLink::saveUserConfig() {
    preferences.begin(MQTT_PREFS_NAMESPACE, false);
    preferences.putString("server", LOCAL_MQTT_BROKER);
    preferences.putString("port", LOCAL_MQTT_PORT);
    preferences.putString("user", LOCAL_MQTT_USER);
    preferences.putString("pass", LOCAL_MQTT_PASSWORD);
    preferences.putString("modbus_mode", LOCAL_MODBUS_MODE);
    preferences.putULong("modbus_baud", LOCAL_MODBUS_BAUD);
    preferences.end();
}
void CommLink::overrideUserConfig(const char* user, const char* pass, const char* server, const char* port, const char* mode, const uint32_t baud) {
    preferences.begin(MQTT_PREFS_NAMESPACE, false);
    preferences.putString("server", server);
    preferences.putString("port", port);
    preferences.putString("user", user);
    preferences.putString("pass", pass);
    preferences.putString("modbus_mode", mode);
    preferences.putULong("modbus_baud", baud);
    preferences.end();
}


auto CommLink::ensureMQTTConnection() const -> bool {
    String clientId = MQTT_CLIENT_PREFIX;
    clientId += String(random(RND_SEED), HEX);
    const bool connected = _mqttClient->connect(clientId.c_str(), LOCAL_MQTT_USER, LOCAL_MQTT_PASSWORD);
    if (!connected) {
        for (int i = 0; i < 3; i++) {
            //setLedColor(true, false, false);
            delay(500);
            _logger->logError(("MQTT connect failed, rc=" + String(_mqttClient->state())).c_str());
        }
    } else {
        //setLedColor(false, false, false);
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

        if (!commLink->_mqttClient->connected()) {
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

void CommLink::networkReset() {
    // AsyncWebServer server(80);
    // DNSServer dns;
    // AsyncWiFiManager wm(&server, &dns, _logger);
    // wm.resetSettings();
    // _logger->logDebug("CommLink::networkReset - WifiManager preferences purged successfully");
    // _logger->logDebug("CommLink::networkReset - Sending restart signal");
    // ESP.restart();
}

void CommLink::setupLED() {
    pinMode(LED_A_PIN, OUTPUT);
    pinMode(LED_B_PIN, OUTPUT);
    pinMode(LED_C_PIN, OUTPUT);
    //setLedColor(false, false, false);
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


