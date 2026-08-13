#include "mqtt/MqttManager.h"

#include "Config.h"
#include "ESPAsyncWebServer.h"
#include "mqtt/MqttConfigDocument.h"
#include <cstdio>
#include <atomic>
#include <utility>
#include "services/IndicatorService.h"
#include "storage/ConfigFs.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

static std::atomic<bool> s_mqttEnabled{false};
static MqttManager *s_activeMqttManager = nullptr;
static  String mqtt_client_prefix = "MBX_CLIENT-";
static constexpr auto MQTT_TASK_STACK = 4096;
static constexpr auto MQTT_TASK_LOOP_DELAY_MS = 100;
static const String system_subscription_network_reset = "/system/network/reset";
static const String system_subscription_echo = "/system/log/echo";

static String buildDefaultClientId() {
    const uint64_t mac = ESP.getEfuseMac();
    char buf[13];
    snprintf(buf, sizeof(buf), "%012llX", static_cast<unsigned long long>(mac));
    return mqtt_client_prefix + String(buf);
}


MqttManager::MqttManager(MqttSubscriptionHandler *subscriptionHandler, PubSubClient *mqttClient, Logger *logger)
    : _mqttClient(mqttClient),
      _logger(logger),
      _mqttTaskHandle(nullptr),
      _subscriptionHandler(subscriptionHandler){
    s_activeMqttManager = this;
}

auto MqttManager::begin() -> bool {
    _mqttClient->setBufferSize(MQTT_BUFFER_SIZE);
    const bool loaded = loadMQTTConfig();
    _mqttClient->setCallback(handleMqttMessage);
    if (loaded) {
        applyServerConfiguration();
        addSystemSubscriptionHandlers(_mqttRootTopic);
    }

    // Do NOT attempt connection here; Wi‑Fi/LWIP may not be initialized yet.
    // The background task will handle connecting once Wi‑Fi is up.
    setMQTTEnabled(loaded && _mqttEnabledConfigured);
    return startMqttTask();
}

bool MqttManager::loadMQTTConfig() {
    clearLoadedConfiguration();
    if (!ConfigFS.exists(ConfigFs::kMqttConfigFile)) {
        _logger->logError("[MQTT] Configuration load failed: mqtt.json is missing");
        return false;
    }

    File configFile = ConfigFS.open(ConfigFs::kMqttConfigFile, FILE_READ);
    if (!configFile) {
        _logger->logError("[MQTT] Configuration load failed: mqtt.json could not be opened");
        return false;
    }
    const String text = configFile.readString();
    configFile.close();

    MqttConfigDocument::StoredConfig stored;
    if (!MqttConfigDocument::parseConfig(text.c_str(), text.length(), stored)) {
        _logger->logError("[MQTT] Configuration load failed: mqtt.json is malformed");
        return false;
    }

    Preferences preferences;
    if (!preferences.begin(MQTT_PREFS_NAMESPACE, false)) {
        _logger->logError("[MQTT] Configuration load failed: password storage is unavailable");
        return false;
    }
    if (preferences.isKey("pass")) {
        const String password = preferences.getString("pass");
        stored.connection.password.assign(password.c_str(), password.length());
    }
    preferences.end();

    MqttConfigCore::PreparedConnection candidate;
    MqttConfigCore::ValidationError validation;
    if (!MqttConfigCore::prepareConnection(stored.connection, candidate, validation)) {
        const String message = String("[MQTT] Configuration load failed: ")
                               + MqttConfigCore::fieldName(validation.field) + " "
                               + MqttConfigCore::constraintMessage(validation);
        _logger->logError(message.c_str());
        return false;
    }

    _mqttConnection = candidate;
    _mqttRootTopic = MqttConfigCore::trim(stored.rootTopic).c_str();
    _mqttEnabledConfigured = stored.enabled;

    _logger->logDebug(("[MQTT] Loaded configuration; User: "
        + String(_mqttConnection.user.data()) + ", Broker: " + String(_mqttConnection.broker.data())
        + ", Port: " + String(_mqttConnection.port) + ", Root Topic: " + _mqttRootTopic).c_str());
    return true;
}

bool MqttManager::applyServerConfiguration() {
    return MqttConfigCore::withValidConnection(
        _mqttConnection, [this](const MqttConfigCore::PreparedConnection &connection) {
        _mqttClient->setServer(connection.broker.data(), connection.port);
        return true;
    });
}

void MqttManager::clearLoadedConfiguration() {
    _mqttConnection = MqttConfigCore::PreparedConnection();
    _mqttRootTopic.clear();
    _mqttEnabledConfigured = false;
    setMQTTEnabled(false);
}


auto MqttManager::ensureMQTTConnection() -> bool {
    if (!_mqttConnection.valid) {
        _logger->logError("[MQTT] Configuration is invalid; skipping connection attempt");
        return false;
    }
    if (_mqttConnection.broker[0] == '\0' || String(_mqttConnection.broker.data()) == MqttConfigCore::DEFAULT_BROKER) {
        _logger->logWarning("[MQTT] Broker not configured; skipping connection attempt");
        return false;
    }
    _logger->logInformation(
        (String("Connecting to MQTT broker [") + _mqttConnection.broker.data() + ":"
         + String(_mqttConnection.port) + "]").c_str());

    String clientId = _clientId;
    if (!clientId.length()) {
        clientId = buildDefaultClientId();
    }
    _clientId = clientId;
    bool connected = false;
    const bool hasUser = (_mqttConnection.user[0] != '\0');
    if (_hasWill && _willTopic.length() && _willMessage.length()) {
        const char *willTopic = _willTopic.c_str();
        const char *willMessage = _willMessage.c_str();
        if (hasUser) {
            connected = _mqttClient->connect(_clientId.c_str(), _mqttConnection.user.data(),
                                             _mqttConnection.password.data(), willTopic, _willQos,
                                             _willRetain, willMessage);
        } else {
            connected = _mqttClient->connect(_clientId.c_str(), willTopic, _willQos, _willRetain, willMessage);
        }
    } else {
        if (hasUser) {
            connected = _mqttClient->connect(_clientId.c_str(), _mqttConnection.user.data(),
                                             _mqttConnection.password.data());
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

void MqttManager::addSystemSubscriptionHandlers(const String &rootTopic) const {
    _subscriptionHandler->addHandler(rootTopic + system_subscription_network_reset, [this](const String &) {
        _logger->logInformation("[MQTT][Subscriptions] Network reset requested by MQTT message");
    });

    _subscriptionHandler->addHandler(rootTopic + system_subscription_echo, [this](const String &msg) {
        _logger->logInformation("[MQTT][Subscriptions] Echo requested");
        _logger->logInformation(msg.c_str());
    });
}

void MqttManager::addSubscriptionHandler(const String &topic, MqttSubscriptionHandler::TopicHandlerFunc handler) const {
    _subscriptionHandler->addHandler(topic, std::move(handler));
    if (_mqttClient->connected()) {
        _mqttClient->subscribe(topic.c_str());
        _logger->logDebug((String("[MQTT][Subscriptions] Subscribed to dynamic topic: ") + topic).c_str());
    }
}

void MqttManager::removeSubscriptionHandlers(const std::vector<String> &topics) const {
    _subscriptionHandler->removeHandlers(topics);
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
    return _mqttConnection.broker.data();
}

int MqttManager::getMQTTState() const {
    return _mqttClient->state();
}

char *MqttManager::getMQTTUser() {
    return _mqttConnection.user.data();
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
    // Load settings and try connecting once, do not start the task. A valid
    // configuration restores the persisted enabled preference after the test;
    // an invalid load remains fail-closed through clearLoadedConfiguration().
    return MqttConfigCore::runOneShotConnectionTest(
        [this](bool &configuredEnabled) {
            if (!loadMQTTConfig()) return false;
            configuredEnabled = _mqttEnabledConfigured;
            return true;
        },
        [this]() {
            if (!applyServerConfiguration()) return false;
            _logger->logInformation((String("Test connect to MQTT [") + _mqttConnection.broker.data()
                                     + ":" + String(_mqttConnection.port) + "]").c_str());
            if (WiFiClass::status() != WL_CONNECTED) {
                _logger->logError("MQTT test connect requested but Wi-Fi not connected");
                return false;
            }
            return ensureMQTTConnection();
        },
        [](const bool configuredEnabled) {
            MqttManager::setMQTTEnabled(configuredEnabled);
        });
}

bool MqttManager::reconfigureFromFile() {
    // Temporarily pause MQTT processing loop
    setMQTTEnabled(false);
    IndicatorService::instance().setMqttConnected(false);
    // Give the task a moment to observe the flag
    vTaskDelay(50 / portTICK_PERIOD_MS);

    // Disconnect if currently connected
    if (_mqttClient->connected()) {
        _mqttClient->disconnect();
    }

    // Remove subscriptions before loading so stale topics cannot remain active.
    _subscriptionHandler->clear();
    if (!loadMQTTConfig() || !applyServerConfiguration()) return false;

    // Rebuild subscriptions for new root topic
    addSystemSubscriptionHandlers(_mqttRootTopic);

    // Resume MQTT processing based on user preference
    setMQTTEnabled(_mqttEnabledConfigured);

    // If Wi-Fi is up and MQTT is enabled, try to connect and resubscribe immediately
    if (isMQTTEnabled() && WiFiClass::status() == WL_CONNECTED) {
        bool connected = false;
        connected = ensureMQTTConnection();
        if (!connected) {
            _logger->logError("[MQTT] Reconfigure failed to connect");
        }
    }
    return true;
}

void MqttManager::setClientId(String clientId) {
    clientId.trim();
    if (!clientId.length()) {
        _clientId.clear();
        return;
    }
    _clientId = clientId;
}

String MqttManager::getClientId() {
    return _clientId;
}
