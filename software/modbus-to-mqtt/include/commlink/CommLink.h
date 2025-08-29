#ifndef COMMLINK_H
#define COMMLINK_H

#include <PubSubClient.h>
#include <WiFiClient.h>
#include <commlink/MqttSubscriptionHandler.h>
#include <Preferences.h>

class CommLink {
public:
    explicit CommLink(MqttSubscriptionHandler *subscriptionHandler, PubSubClient *mqttClient, Logger *logger);

    auto begin() -> bool;

    auto ensureMQTTConnection() const -> bool;

    auto mqttPublish(const char *topic, const char *payload) const -> bool;

    void onMqttMessage(const String& topic, const uint8_t *payload, size_t length) const;

    auto startMqttTask() -> bool;

    void networkReset();

    static void setupLED();

    auto getMqttBroker() -> char*;

    auto getMQTTState() const -> int;

    auto getMQTTUser() -> char *;

    void overrideUserConfig(const char *user, const char *pass, const char *server, const char *port, const char *mode,
                        uint32_t baud);
    static void setMQTTEnabled(bool enabled);
    static auto isMQTTEnabled() -> bool;

private:
    [[noreturn]] static void processMQTTAsync(void *parameter);

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
    Preferences preferences;
};

#endif //COMMLINK_H
