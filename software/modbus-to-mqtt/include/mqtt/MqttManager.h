#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <PubSubClient.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mqtt/MqttSubscriptionHandler.h>

#include "Config.h"
#include "concurrency/LatestRevisionCore.h"
#include "concurrency/StagedCommitCore.h"
#include "mqtt/MqttRuntimeTypes.h"

// Ownership contract:
// - setup() may initialize the client before startMqttTask() returns.
// - after that point processMQTTAsync is the sole owner of PubSubClient, the
//   active MQTT configuration and the subscription registry.
// - every other task submits an owned command or reads a copied snapshot.
class MqttManager {
public:
    explicit MqttManager(MqttSubscriptionHandler *subscriptionHandler, PubSubClient *mqttClient, Logger *logger);

    static void handleMqttMessage(char *topic, const byte *payload, unsigned int length);

    bool begin();

    MqttOperationResult publish(const MqttPublishRequest &request);

    MqttAdmissionResult publishAsync(const MqttPublishRequest &request,
                                     MqttPublishCallback callback = {});

    MqttOperationResult stageBridgePlan(MqttBridgePlan plan);

    void approveBridgePlan(uint32_t modbusGeneration);

    MqttOperationResult setEnabled(bool enabled);

    MqttStatusSnapshot getStatusSnapshot() const;

    bool isConnected() const;

    bool reconfigureFromFile();

    MqttOperationResult requestReconfigureFromFile();

    MqttOperationResult requestReconfigure(const String &configurationJson,
                                           uint32_t revision);

    uint32_t issueConfigurationRevision();

    bool isConfigurationRevisionObsolete(uint32_t revision) const;

    MqttTestResult testConnectOnce();

    static void setMQTTEnabled(bool enabled);

    static bool isMQTTEnabled();

private:
    struct Command;

    [[noreturn]] static void processMQTTAsync(void *parameter);

    bool startMqttTask();

    bool loadMQTTConfig(MqttRuntimeConfiguration &candidate) const;

    bool parseMQTTConfig(const char *json, size_t length,
                         MqttRuntimeConfiguration &candidate) const;

    MqttOperationResult submitAndWait(Command *command, uint32_t waitMs = MQTT_COMMAND_WAIT_MS,
                                      MqttTestResult *testResult = nullptr,
                                      LatestRevisionCore::Admission *admission = nullptr);

    MqttAdmissionResult submitAsync(Command *command);

    void processCommand(Command &command);

    void applyConfiguration(MqttRuntimeConfiguration configuration, uint32_t revision,
                            MqttOperationResult &result);

    void stageBridgePlanOwned(MqttBridgePlan plan, MqttOperationResult &result);

    void commitApprovedBridgePlan();

    void applyEnabled(bool enabled, MqttOperationResult &result);

    void executePublish(Command &command);

    void executeConnectionTest(Command &command);

    bool ensureMQTTConnection();

    bool connectWithConfiguration(const MqttRuntimeConfiguration &configuration,
                                  const MqttWillSpec &will,
                                  bool subscribeAfterConnect);

    void disconnectOwned();

    void rebuildOwnedSubscriptions(bool reconcileBroker);

    std::vector<MqttSubscriptionHandler::HandlerEntry> buildHandlerEntries() const;

    String resolveTopic(const MqttTopicSpec &topic) const;

    static String resolveTopicForRoot(const MqttTopicSpec &topic, const String &rootTopic);

    void onMqttMessage(const String &topic, const uint8_t *payload, size_t length) const;

    void updateStatusSnapshot();

    bool inOwnerContext() const;

    void recordOwnerViolation();

    MqttRuntimeConfiguration _activeConfiguration;
    MqttBridgePlan _bridgePlan;
    StagedCommitCore<MqttBridgePlan> _stagedBridgePlan;
    LatestRevisionCore _configurationRevisions;
    bool _runtimeEnabled{true};
    uint32_t _generation{0U};
    uint32_t _connectionEpoch{0U};

    PubSubClient *_mqttClient;
    Logger *_logger;
    TaskHandle_t _mqttTaskHandle{nullptr};
    QueueHandle_t _commandQueue{nullptr};
    mutable SemaphoreHandle_t _statusMutex{nullptr};
    MqttStatusSnapshot _statusSnapshot;
    MqttSubscriptionHandler *_subscriptionHandler;
    std::atomic<bool> _taskRunning{false};
    std::atomic<bool> _shuttingDown{false};
    std::atomic<uint32_t> _ownerViolationCount{0U};
    std::atomic<uint32_t> _commandFailureCount{0U};
};

#endif
