#include "commlink/CommLink.h"

#include <commlink/MqttSubscriptions.h>
#include "Config.h"

static CommLink *s_activeCommLink = nullptr;

CommLink::CommLink(MqttSubscriptionHandler *subscriptionHandler, PubSubClient *mqttClient, Logger *logger)
    : _mqttClient(mqttClient), _logger(logger), _mqttTaskHandle(nullptr), _subscriptionHandler(subscriptionHandler) {
    s_activeCommLink = this;
    s_activeCommLink = this;
}

void handleMqttMessage(char *topic, byte *payload, unsigned int length) {
    if (s_activeCommLink) {
        s_activeCommLink->onMqttMessage(topic, payload, length);
    }
}

bool CommLink::begin() {
    setupLED();
    checkResetButton();
    wifiSetup();

    const char *ipAddr = {LOCAL_MQTT_SERVER_IP};
    _logger->logInformation(
        ("Connecting to MQTT broker [" + String(ipAddr) + ":" + String(LOCAL_MQTT_PORT) + "]").c_str());
    _mqttClient->setServer(ipAddr, atoi(LOCAL_MQTT_PORT));
    if (!ensureMQTTConnection())
        _logger->logError("MQTT connection failed");
    _mqttClient->setCallback(handleMqttMessage);
    return startMqttTask();
}

void CommLink::wifiSetup() {
    loadMQTTConfig();

    WiFiManagerParameter p_mqtt_server("server", "MQTT Server", LOCAL_MQTT_SERVER_IP, 40);
    WiFiManagerParameter p_mqtt_port("port", "MQTT Port", LOCAL_MQTT_PORT, 6);
    WiFiManagerParameter p_mqtt_user("user", "MQTT Username", LOCAL_MQTT_USER, 32);
    WiFiManagerParameter p_mqtt_pass("pass", "MQTT Password", LOCAL_MQTT_PASSWORD, 32);

    wm.addParameter(&p_mqtt_server);
    wm.addParameter(&p_mqtt_port);
    wm.addParameter(&p_mqtt_user);
    wm.addParameter(&p_mqtt_pass);

    const unsigned long startAttemptTime = millis();
    while (!wm.autoConnect(DEFAULT_AP_SSID, DEFAULT_AP_PASS)) {
        if (millis() - startAttemptTime > WIFI_CONNECT_TIMEOUT) {
            ESP.restart();
        }
        delay(100);
    }

    strcpy(LOCAL_MQTT_SERVER_IP, p_mqtt_server.getValue());
    strcpy(LOCAL_MQTT_PORT, p_mqtt_port.getValue());
    strcpy(LOCAL_MQTT_USER, p_mqtt_user.getValue());
    strcpy(LOCAL_MQTT_PASSWORD, p_mqtt_pass.getValue());

    saveMQTTConfig();
}

void CommLink::loadMQTTConfig() {
    prefs.begin(MQTT_PREFS_NAMESPACE, false);

    if (prefs.isKey("server")) {
        strcpy(LOCAL_MQTT_SERVER_IP, prefs.getString("server").c_str());
    } else {
        strcpy(LOCAL_MQTT_SERVER_IP, DEFAULT_MQTT_BROKER_IP);
    }

    if (prefs.isKey("port")) {
        strcpy(LOCAL_MQTT_PORT, prefs.getString("port").c_str());
    } else {
        strcpy(LOCAL_MQTT_PORT, DEFAULT_MQTT_BROKER_PORT);
    }

    if (prefs.isKey("user")) {
        strcpy(LOCAL_MQTT_USER, prefs.getString("user").c_str());
    } else {
        LOCAL_MQTT_USER[0] = '\0';
    }

    if (prefs.isKey("pass")) {
        strcpy(LOCAL_MQTT_PASSWORD, prefs.getString("pass").c_str());
    } else {
        LOCAL_MQTT_PASSWORD[0] = '\0';
    }
    prefs.end();
}

void CommLink::saveMQTTConfig() {
    prefs.begin(MQTT_PREFS_NAMESPACE, false);
    prefs.putString("server", LOCAL_MQTT_SERVER_IP);
    prefs.putString("port", LOCAL_MQTT_PORT);
    prefs.putString("user", LOCAL_MQTT_USER);
    prefs.putString("pass", LOCAL_MQTT_PASSWORD);
    prefs.end();
}

bool CommLink::ensureMQTTConnection() {
    String clientId = MQTT_CLIENT_PREFIX;
    clientId += String(random(0xffff), HEX);
    const bool connected = _mqttClient->connect(clientId.c_str(), LOCAL_MQTT_USER, LOCAL_MQTT_PASSWORD);
    if (!connected) {
        for (int i = 0; i < 3; i++) {
            setLedColor(true, false, false);
            delay(250);
            setLedColor(false, false, false);
            delay(250);
        }
    } else {
        setLedColor(false, false, false);
    }
    _logger->logError(("MQTT connect failed, rc=" + String(_mqttClient->state())).c_str());

    //todo: use subscriptionhandler to find subscribed topics

    _mqttClient->subscribe((MQTT_ROOT_TOPIC + MQTT_SUB_NETWORK_RESET).c_str());
    _logger->logInformation(("MQTT subscribe to: " + String(MQTT_ROOT_TOPIC) + String(MQTT_SUB_NETWORK_RESET)).c_str());
    return connected;
}

[[noreturn]] void CommLink::processMQTTAsync(void *parameter) {
    auto *commLink = static_cast<CommLink *>(parameter);
    constexpr TickType_t delayTicks = MQTT_TASK_LOOP_DELAY_MS / portTICK_PERIOD_MS;
    static unsigned long lastReconnectAttempt = 0;
    while (true) {
        if (!commLink->_mqttClient->connected()) {
            const unsigned long now = millis();
            if (now - lastReconnectAttempt >= MQTT_RECONNECT_INTERVAL_MS) {
                lastReconnectAttempt = now;
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

bool CommLink::mqttPublish(const char *topic, const char *payload) {
    return _mqttClient->publish(topic, payload);
}

void CommLink::onMqttMessage(const char *topic, const uint8_t *payload, size_t length) {
    String message;
    _logger->logInformation("CommLink::onMqttMessage - Received MQTT message");

    message.reserve(length);
    for (size_t i = 0; i < length; i++) {
        message += static_cast<char>(payload[i]);
    }
    _subscriptionHandler->handle(topic, message);
}


void CommLink::networkReset() {
    wm.resetSettings();
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
        _logger->logInformation("CommLink::checkResetButton - Button press detected");

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

                setLedColor(true, false, false); // Red ON

                prefs.begin(MQTT_PREFS_NAMESPACE, false);
                prefs.clear();
                prefs.end();

                wm.resetSettings();

                delay(1000);
                _logger->logInformation("CommLink::checkResetButton - Settings cleared, rebooting...");
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
