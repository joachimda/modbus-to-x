#ifndef COMMLINK_H
#define COMMLINK_H

#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <commlink/MqttSubscriptionHandler.h>
#include <Preferences.h>

class CommLink {
public:
    explicit CommLink(MqttSubscriptionHandler *subscriptionHandler, PubSubClient *mqttClient, Logger *logger);

    bool begin();

    bool ensureMQTTConnection() const;

    bool mqttPublish(const char *topic, const char *payload) const;

    void onMqttMessage(const String& topic, const uint8_t *payload, size_t length) const;

    bool startMqttTask();

    void networkReset();

    static void setupLED();

    static void setLedColor(bool red, bool green, bool blue);

    void checkResetButton();

    char* getMqttBroker();

    int getMQTTState() const;

    char *getMQTTUser();

private:
    [[noreturn]] static void processMQTTAsync(void *parameter);

    void wifiSetup();

    void loadMQTTConfig();

    void saveUserConfig();

    char LOCAL_MQTT_BROKER[150] = "";
    char LOCAL_MQTT_PORT[6] = "";
    char LOCAL_MQTT_USER[32] = "";
    char LOCAL_MQTT_PASSWORD[32] = "";
    char LOCAL_MODBUS_MODE[3] = "";
    int LOCAL_MODBUS_BAUD = 0;
    PubSubClient *_mqttClient;
    Logger *_logger;
    TaskHandle_t _mqttTaskHandle;
    MqttSubscriptionHandler *_subscriptionHandler;
    WiFiManager wm;
    Preferences preferences;
};

#endif //COMMLINK_H
