#include "mqtt/MqttManager.h"

#include "Config.h"
#include "ESPAsyncWebServer.h"
#include <atomic>
#include "services/IndicatorService.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>

static std::atomic<bool> s_mqttEnabled{false};
static MqttManager *s_activeMqttManager = nullptr;
static  String mqtt_client_prefix = "MBX_CLIENT-";
static constexpr auto MQTT_TASK_STACK = 4096;
static constexpr auto MQTT_TASK_LOOP_DELAY_MS = 100;
static constexpr auto RND_SEED = 0xffff;
static constexpr auto default_mqtt_broker = "0.0.0.0";
static constexpr auto default_mqtt_port = "1883";
static constexpr auto default_mqtt_root_topic = "mbx_root";
static const String system_subscription_network_reset = "/system/network/reset";
static const String system_subscription_echo = "/system/log/echo";

MqttManager::MqttManager(MqttSubscriptionHandler *subscriptionHandler, PubSubClient *mqttClient, Logger *logger)
    : _mqttClient(mqttClient),
      _logger(logger),
      _mqttTaskHandle(nullptr),
      _subscriptionHandler(subscriptionHandler){
    s_activeMqttManager = this;
}

auto MqttManager::begin() -> bool {
    _mqttClient->setBufferSize(MQTT_BUFFER_SIZE);
    loadMQTTConfig();

    const char *broker = {_mqttBroker};
    _mqttClient->setServer(broker, atoi(_mqttPort));
    _mqttClient->setCallback(handleMqttMessage);
    addSubscriptionHandlers(_mqttRootTopic);

    // Do NOT attempt connection here; Wi‑Fi/LWIP may not be initialized yet.
    // The background task will handle connecting once Wi‑Fi is up.
    setMQTTEnabled(true);
    return startMqttTask();
}

void MqttManager::loadMQTTConfig() {
    String server = default_mqtt_broker;
    String user, port, rootTopic = "";
    if (SPIFFS.exists("/conf/mqtt.json")) {
        File config_file = SPIFFS.open("/conf/mqtt.json", FILE_READ);
        if (config_file) {
            String text = config_file.readString();
            config_file.close();
            JsonDocument doc;
            if (!deserializeJson(doc, text)) {
                String ip_from_file = doc["broker_ip"] | "";
                String url_from_file = doc["broker_url"] | "";
                String port_from_file = doc["broker_port"] | default_mqtt_port;
                String user_from_file = doc["user"] | "";
                String root_topic_from_file = doc["root_topic"] | default_mqtt_root_topic;
                auto extractHost = [](const String &uurl) -> String {
                    if (uurl.length() == 0) {
                        return {""};
                    }
                    int start = uurl.indexOf("://");
                    start = (start >= 0) ? (start + 3) : 0;
                    const int slash = uurl.indexOf('/', start);
                    const int colon = uurl.indexOf(':', start);
                    unsigned int end;
                    if (slash >= 0 && colon >= 0) {
                        end = slash < colon ? slash : colon;
                    } else if (slash >= 0) {
                        end = slash;
                    } else if (colon >= 0) {
                        end = colon;
                    } else {
                        end = uurl.length();
                    }
                    return uurl.substring(start, end);
                };
                ip_from_file.trim();
                url_from_file.trim();
                port_from_file.trim();
                user_from_file.trim();
                root_topic_from_file.trim();
                if (ip_from_file.length() && ip_from_file != default_mqtt_broker) server = ip_from_file;
                else if (url_from_file.length()) {
                    server = extractHost(url_from_file);
                }
                if (port_from_file.length()) {
                    port = port_from_file;
                }
                user = user_from_file;
                rootTopic = root_topic_from_file;
            }
        }
    }
    _mqttRootTopic = rootTopic;
    strcpy(_mqttBroker, server.c_str());
    strcpy(_mqttPort, port.c_str());
    if (user.length()) {
        strcpy(_mqttUser, user.c_str());
    }
    else {
        _mqttUser[0] = '\0';
    }

    _logger->logDebug(("[MQTT] Loaded configuration; User: "
        + String(_mqttUser) + ", Broker: " + String(_mqttBroker)
        + ", Port: " + String(_mqttPort) + ", Root Topic: " + String(_mqttRootTopic)  ).c_str());

    preferences.begin(MQTT_PREFS_NAMESPACE, false);
    if (preferences.isKey("pass")) {
        strcpy(_mqttPassword, preferences.getString("pass").c_str());
    } else {
        _mqttPassword[0] = '\0';
    }
    preferences.end();
}


auto MqttManager::ensureMQTTConnection() -> bool {
    const auto clientId = String(mqtt_client_prefix + String(random(RND_SEED), HEX));
    if (_mqttBroker[0] == '\0' || String(_mqttBroker) == default_mqtt_broker) {
        _logger->logWarning("[MQTT] Broker not configured; skipping connection attempt");
        return false;
    }
    _logger->logInformation(
        (String("Connecting to MQTT broker [") + _mqttBroker + ":" + String(_mqttPort) + "]").c_str());

    _clientId = clientId;
    bool connected = false;
    const bool hasUser = (_mqttUser[0] != '\0');
    if (_hasWill && _willTopic.length() && _willMessage.length()) {
        const char *willTopic = _willTopic.c_str();
        const char *willMessage = _willMessage.c_str();
        if (hasUser) {
            connected = _mqttClient->connect(_clientId.c_str(), _mqttUser, _mqttPassword, willTopic, _willQos, _willRetain, willMessage);
        } else {
            connected = _mqttClient->connect(_clientId.c_str(), willTopic, _willQos, _willRetain, willMessage);
        }
    } else {
        if (hasUser) {
            connected = _mqttClient->connect(_clientId.c_str(), _mqttUser, _mqttPassword);
        } else {
            connected = _mqttClient->connect(_clientId.c_str());
        }
    }

    if (!connected) {
        _logger->logError((String("MQTT connect failed, rc=") + String(_mqttClient->state())).c_str());
    } else {
        IndicatorService::instance().setMqttConnected(true);
    }

    for (const auto &topic: _subscriptionHandler->getHandlerTopics()) {
        _mqttClient->subscribe(topic.c_str());
        _logger->logInformation(("MQTT subscribe to: " + topic).c_str());
    }
    return connected;
}

void MqttManager::handleMqttMessage(char *topic, const byte *payload, const unsigned int length) {
    if (s_activeMqttManager != nullptr) {
        const auto topicStr = String(topic);
        s_activeMqttManager->onMqttMessage(topicStr, payload, length);
    }
}

void MqttManager::addSubscriptionHandlers(const String &rootTopic) const {
    _subscriptionHandler->addHandler(rootTopic + system_subscription_network_reset, [this](const String &) {
        _logger->logInformation("[MQTT][Subscriptions] Network reset requested by MQTT message");
    });

    _subscriptionHandler->addHandler(rootTopic + system_subscription_echo, [this](const String &msg) {
        _logger->logInformation("[MQTT][Subscriptions] Echo requested");
        _logger->logInformation(msg.c_str());
    });
}

[[noreturn]] void MqttManager::processMQTTAsync(void *parameter) {
    auto *mqtt_manager = static_cast<MqttManager *>(parameter);
    constexpr TickType_t delayTicks = MQTT_TASK_LOOP_DELAY_MS / portTICK_PERIOD_MS;
    static unsigned long lastReconnectAttempt = 0;
    while (true) {
        if (!isMQTTEnabled()) {
            vTaskDelay(delayTicks);
            continue;
        }

        // Wi‑Fi gates interactions with MQTT
        if (WiFiClass::status() != WL_CONNECTED) {
            IndicatorService::instance().setMqttConnected(false);
            vTaskDelay(delayTicks);
            continue;
        }

        const bool connectedNow = mqtt_manager->_mqttClient->connected();
        IndicatorService::instance().setMqttConnected(connectedNow);
        if (!connectedNow) {
            mqtt_manager->_logger->logError("MQTT disconnected, attempting reconnect");
            const unsigned long now = millis();
            if (now - lastReconnectAttempt >= MQTT_RECONNECT_INTERVAL_MS) {
                lastReconnectAttempt = now;
                if (!mqtt_manager->ensureMQTTConnection()) {
                    mqtt_manager->_logger->logError("MQTT reconnect attempt failed in task loop");
                }
            }
        }
        mqtt_manager->_mqttClient->loop();
        vTaskDelay(delayTicks);
    }
}

bool MqttManager::startMqttTask() {
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

bool MqttManager::mqttPublish(const char *topic, const char *payload, const bool retain) const {
    if (!_mqttClient) {
        return false;
    }
    return _mqttClient->publish(topic, payload, retain);
}

void MqttManager::configureWill(const String &topic, const String &payload, const uint8_t qos, const bool retain) {
    _willTopic = topic;
    _willTopic.trim();
    _willMessage = payload;
    _willMessage.trim();
    _willQos = qos;
    _willRetain = retain;
    _hasWill = _willTopic.length() && _willMessage.length();
}

void MqttManager::clearWill() {
    _willTopic.clear();
    _willMessage.clear();
    _willQos = 0;
    _willRetain = false;
    _hasWill = false;
}

bool MqttManager::isConnected() const {
    return _mqttClient->connected();
}

void MqttManager::onMqttMessage(const String &topic, const uint8_t *payload, const size_t length) const {
    String message;
    message.reserve(length);
    for (size_t i = 0; i < length; i++) {
        message += static_cast<char>(payload[i]);
    }
    _subscriptionHandler->handle(topic, message);
    _logger->logDebug("MqttManager::onMqttMessage - Received MQTT message");
}

char *MqttManager::getMqttBroker() {
    return _mqttBroker;
}

int MqttManager::getMQTTState() const {
    return _mqttClient->state();
}

char *MqttManager::getMQTTUser() {
    return _mqttUser;
}

const String &MqttManager::getRootTopic() const {
    return _mqttRootTopic;
}

void MqttManager::setMQTTEnabled(const bool enabled) {
    s_mqttEnabled.store(enabled, std::memory_order_release);
}

bool MqttManager::isMQTTEnabled() {
    return s_mqttEnabled.load(std::memory_order_acquire);
}

bool MqttManager::testConnectOnce() {
    // Load settings and try connecting once, do not start the task
    loadMQTTConfig();
    const char *broker = {_mqttBroker};
    _logger->logInformation((String("Test connect to MQTT [") + broker + ":" + String(_mqttPort) + "]").c_str());
    _mqttClient->setServer(broker, atoi(_mqttPort));
    if (WiFiClass::status() != WL_CONNECTED) {
        _logger->logError("MQTT test connect requested but Wi-Fi not connected");
        return false;
    }
    return ensureMQTTConnection();
}

void MqttManager::reconfigureFromFile() {
    // Temporarily pause MQTT processing loop
    setMQTTEnabled(false);
    IndicatorService::instance().setMqttConnected(false);
    // Give the task a moment to observe the flag
    vTaskDelay(50 / portTICK_PERIOD_MS);

    // Disconnect if currently connected
    if (_mqttClient->connected()) {
        _mqttClient->disconnect();
    }

    // Reload configuration from SPIFFS/NVS
    loadMQTTConfig();

    // Point client to new broker/port
    const char *broker = {_mqttBroker};
    _mqttClient->setServer(broker, atoi(_mqttPort));

    // Rebuild subscriptions for new root topic
    _subscriptionHandler->clear();
    addSubscriptionHandlers(_mqttRootTopic);

    // Resume MQTT processing
    setMQTTEnabled(true);

    // If Wi-Fi is up, try to connect and resubscribe immediately
    if (WiFiClass::status() == WL_CONNECTED) {
        bool connected = false;
        connected = ensureMQTTConnection();
        if (!connected) {
            _logger->logError("[MQTT] Reconfigure failed to connect");
        }
    }
}
