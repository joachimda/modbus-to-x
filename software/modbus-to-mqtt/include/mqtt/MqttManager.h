#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <PubSubClient.h>
#include <mqtt/MqttSubscriptionHandler.h>
#include <Preferences.h>

class MqttManager {
public:
    explicit MqttManager(MqttSubscriptionHandler *subscriptionHandler, PubSubClient *mqttClient, Logger *logger);

    static void handleMqttMessage(char *topic, const byte *payload, unsigned int length);

    void addSubscriptionHandlers(const String &rootTopic) const;

    auto begin() -> bool;

    auto ensureMQTTConnection() const -> bool;

    auto mqttPublish(const char *topic, const char *payload) const -> bool;

    void onMqttMessage(const String& topic, const uint8_t *payload, size_t length) const;

    auto startMqttTask() -> bool;

    auto getMqttBroker() -> char*;

    auto getMQTTState() const -> int;

    auto getMQTTUser() -> char *;

    static void setMQTTEnabled(bool enabled);
    static auto isMQTTEnabled() -> bool;

    auto testConnectOnce() -> bool;

    void reconfigureFromFile();

private:
    [[noreturn]] static void processMQTTAsync(void *parameter);

    void loadMQTTConfig();

    char _mqttBroker[150] = "";
    char _mqttPort[6] = "";
    char _mqttUser[32] = "";
    char _mqttPassword[32] = "";
    String _mqttRootTopic = "";
    String _clientId = "";

    PubSubClient *_mqttClient;
    Logger *_logger;
    TaskHandle_t _mqttTaskHandle;
    MqttSubscriptionHandler *_subscriptionHandler;
    Preferences preferences;
};

#endif
