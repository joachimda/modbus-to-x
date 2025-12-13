#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <PubSubClient.h>
#include <mqtt/MqttSubscriptionHandler.h>
#include <Preferences.h>

class MqttManager {
public:
    explicit MqttManager(MqttSubscriptionHandler *subscriptionHandler, PubSubClient *mqttClient, Logger *logger);

    static void handleMqttMessage(char *topic, const byte *payload, unsigned int length);

    void addSystemSubscriptionHandlers(const String &rootTopic) const;

    auto begin() -> bool;

    auto ensureMQTTConnection() -> bool;

    auto mqttPublish(const char *topic, const char *payload, bool retain = false) const -> bool;

    void configureWill(const String &topic, const String &payload, uint8_t qos, bool retain);

    void clearWill();

    auto isConnected() const -> bool;

    void onMqttMessage(const String &topic, const uint8_t *payload, size_t length) const;

    auto startMqttTask() -> bool;

    auto getMqttBroker() -> char *;

    auto getMQTTState() const -> int;

    auto getMQTTUser() -> char *;

    auto getRootTopic() const -> const String &;

    static void setMQTTEnabled(bool enabled);

    static auto isMQTTEnabled() -> bool;

    auto testConnectOnce() -> bool;

    void reconfigureFromFile();

    String getClientId();

private:
    [[noreturn]] static void processMQTTAsync(void *parameter);

    void loadMQTTConfig();

    void setClientId(String clientId);

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
    bool _hasWill{false};
    String _willTopic;
    String _willMessage;
    uint8_t _willQos{0};
    bool _willRetain{false};
};

#endif
