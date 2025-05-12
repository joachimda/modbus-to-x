#include "commlink/CommLink.h"
#include "Config.h"

static CommLink *s_activeCommLink = nullptr;
static constexpr auto DEFAULT_MQTT_BROKER_PORT = "1883";
static constexpr auto DEFAULT_MQTT_BROKER_IP = "0.0.0.0";
static constexpr auto DEFAULT_MODBUS_MODE = "8N1";
static constexpr auto MQTT_CLIENT_PREFIX = "MODBUS_CLIENT-";
static constexpr auto WIFI_CONNECT_TIMEOUT = 10000;
static constexpr auto MQTT_TASK_STACK = 4096;
static constexpr auto MQTT_TASK_LOOP_DELAY_MS = 100;

CommLink::CommLink(MqttSubscriptionHandler *subscriptionHandler, PubSubClient *mqttClient, Logger *logger)
    : _mqttClient(mqttClient), _logger(logger), _mqttTaskHandle(nullptr), _subscriptionHandler(subscriptionHandler) {
    s_activeCommLink = this;
    s_activeCommLink = this;
}

void handleMqttMessage(char *topic, const byte *payload, const unsigned int length) {
    if (s_activeCommLink) {
        const auto topicStr = String(topic);
        s_activeCommLink->onMqttMessage(topicStr, payload, length);
    }
}

bool CommLink::begin() {
    setupLED();
    checkResetButton();
    wifiSetup();

    _mqttClient->setBufferSize(4096);
    const char *broker = {LOCAL_MQTT_BROKER};
    _logger->logInformation(
        ("Connecting to MQTT broker [" + String(broker) + ":" + String(LOCAL_MQTT_PORT) + "]").c_str());
    _mqttClient->setServer(broker, atoi(LOCAL_MQTT_PORT));
    if (!ensureMQTTConnection())
        _logger->logError("MQTT connection failed");
    _mqttClient->setCallback(handleMqttMessage);

    Serial.print("MQTT Buffer size: "); _mqttClient->getBufferSize();
    Serial.println(_mqttClient->getBufferSize());
    return startMqttTask();
}

void CommLink::wifiSetup() {
    loadMQTTConfig();

    WiFiManagerParameter p_mqtt_broker("server", "MQTT Broker domain/IP", LOCAL_MQTT_BROKER, 150);
    WiFiManagerParameter p_mqtt_port("port", "MQTT Broker Port", LOCAL_MQTT_PORT, 6);
    WiFiManagerParameter p_mqtt_user("user", "MQTT Username", LOCAL_MQTT_USER, 32);
    WiFiManagerParameter p_mqtt_pass("pass", "MQTT Password", LOCAL_MQTT_PASSWORD, 32);
    WiFiManagerParameter p_modbus_mode("modbus_mode", "MODBUS Mode",DEFAULT_MODBUS_MODE , 3);

    wm.addParameter(&p_mqtt_broker);
    wm.addParameter(&p_mqtt_port);
    wm.addParameter(&p_mqtt_user);
    wm.addParameter(&p_mqtt_pass);
    wm.addParameter(&p_modbus_mode);

    const unsigned long startAttemptTime = millis();
    while (!wm.autoConnect(DEFAULT_AP_SSID, DEFAULT_AP_PASS)) {
        if (millis() - startAttemptTime > WIFI_CONNECT_TIMEOUT) {
            ESP.restart();
        }
        delay(100);
    }

    strcpy(LOCAL_MQTT_BROKER, p_mqtt_broker.getValue());
    strcpy(LOCAL_MQTT_PORT, p_mqtt_port.getValue());
    strcpy(LOCAL_MQTT_USER, p_mqtt_user.getValue());
    strcpy(LOCAL_MQTT_PASSWORD, p_mqtt_pass.getValue());

    saveUserConfig();
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

    if (preferences.isKey("modbus_mode")) {
        strcpy(LOCAL_MODBUS_MODE, preferences.getString("modbus_mode").c_str());
    } else {
        strcpy(LOCAL_MODBUS_MODE, DEFAULT_MODBUS_MODE);
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
    preferences.end();
}

bool CommLink::ensureMQTTConnection() const {
    String clientId = MQTT_CLIENT_PREFIX;
    clientId += String(random(0xffff), HEX);
    const bool connected = _mqttClient->connect(clientId.c_str(), LOCAL_MQTT_USER, LOCAL_MQTT_PASSWORD);
    if (!connected) {
        for (int i = 0; i < 3; i++) {
            setLedColor(true, false, false);
            delay(250);
            setLedColor(false, false, false);
            delay(250);
            _logger->logError(("MQTT connect failed, rc=" + String(_mqttClient->state())).c_str());
        }
    } else {
        setLedColor(false, false, false);
    }

    for (const auto& topic : _subscriptionHandler->getHandlerTopics()) {
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
    wm.resetSettings();
    _logger->logDebug("CommLink::networkReset - WifiManager preferences purged successfully");
    _logger->logDebug("CommLink::networkReset - Sending restart signal");
    ESP.restart();
}

void CommLink::setupLED() {
    pinMode(RGB_LED_RED_PIN, OUTPUT);
    pinMode(RGB_LED_GREEN_PIN, OUTPUT);
    pinMode(RGB_LED_BLUE_PIN, OUTPUT);
    setLedColor(false, false, false);
}

void CommLink::setLedColor(const bool red, const bool green, const bool blue) {
    digitalWrite(RGB_LED_RED_PIN, red ? HIGH : LOW);
    digitalWrite(RGB_LED_GREEN_PIN, green ? HIGH : LOW);
    digitalWrite(RGB_LED_BLUE_PIN, blue ? HIGH : LOW);
}


void CommLink::checkResetButton() {
    pinMode(RESET_BUTTON_PIN, INPUT);

    if (digitalRead(RESET_BUTTON_PIN) == HIGH) {
        _logger->logInformation("CommLink::checkResetButton - Reset Button press detected");

        const unsigned long pressStart = millis();

        while (digitalRead(RESET_BUTTON_PIN) == HIGH) {
            const unsigned long heldTime = millis() - pressStart;

            if (heldTime / 200 % 2 == 0) {
                setLedColor(false, false, true);
            } else {
                setLedColor(false, false, false);
            }

            if (heldTime >= RESET_HOLD_TIME_MS) {
                _logger->logInformation("CommLink::checkResetButton - Reset confirmed: clearing settings");

                setLedColor(true, false, false);

                preferences.begin(MQTT_PREFS_NAMESPACE, false);
                preferences.clear();
                preferences.end();
                networkReset();

                delay(1000);
                ESP.restart();
            }
            delay(50);
        }

        _logger->logWarning("Reset cancelled");
        setLedColor(false, true, false);
        delay(500);
        setLedColor(false, false, false);
    }
}

char * CommLink::getMqttBroker() {
    return LOCAL_MQTT_BROKER;
}

int CommLink::getMQTTState() const {
    return _mqttClient->state();
}

char *CommLink::getMQTTUser() {
    return LOCAL_MQTT_USER;
}
