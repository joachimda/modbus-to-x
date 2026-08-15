#include "mqtt/MqttManager.h"

#include <Preferences.h>
#include <WiFi.h>
#include <algorithm>
#include <new>
#include <utility>

#include "Config.h"
#include "concurrency/OwnerRequest.h"
#include "mqtt/MqttConfigDocument.h"
#include "services/IndicatorService.h"
#include "storage/ConfigFs.h"

namespace {

MqttManager *s_activeMqttManager = nullptr;
constexpr auto MQTT_TASK_STACK = 6144;
constexpr auto MQTT_TASK_LOOP_DELAY_MS = 50;
const String MQTT_CLIENT_PREFIX = "MBX_CLIENT-";
const String SYSTEM_NETWORK_RESET = "/system/network/reset";
const String SYSTEM_ECHO = "/system/log/echo";

String buildDefaultClientId() {
    const uint64_t mac = ESP.getEfuseMac();
    char buffer[13];
    snprintf(buffer, sizeof(buffer), "%012llX", static_cast<unsigned long long>(mac));
    return MQTT_CLIENT_PREFIX + String(buffer);
}

bool containsTopic(const std::vector<String> &topics, const String &topic) {
    return std::find(topics.begin(), topics.end(), topic) != topics.end();
}

}  // namespace

struct MqttManager::Command final : OwnerRequest {
    enum class Type : uint8_t {
        Publish,
        ApplyConfiguration,
        ApplyBridgePlan,
        SetEnabled,
        TestConnection,
    };

    explicit Command(const Type commandType)
        : type(commandType) {
    }

    Type type;
    MqttOperationResult result{MqttOperationResult::Unavailable};
    MqttRuntimeConfiguration configuration;
    MqttBridgePlan bridgePlan;
    MqttPublishRequest publishRequest;
    MqttPublishCallback publishCallback;
    MqttTestResult testResult;
    uint32_t configurationRevision{0U};
    bool enabled{false};
    bool asynchronous{false};
};

MqttManager::MqttManager(MqttSubscriptionHandler *subscriptionHandler, PubSubClient *mqttClient, Logger *logger)
    : _mqttClient(mqttClient),
      _logger(logger),
      _subscriptionHandler(subscriptionHandler) {
    s_activeMqttManager = this;
}

bool MqttManager::begin() {
    _statusMutex = xSemaphoreCreateMutex();
    _commandQueue = xQueueCreate(MQTT_COMMAND_QUEUE_DEPTH, sizeof(Command *));
    if (_statusMutex == nullptr || _commandQueue == nullptr || _mqttClient == nullptr || _subscriptionHandler == nullptr) {
        _logger->logError("[MQTT] Failed to initialize owner mailbox or status snapshot");
        return false;
    }

    _mqttClient->setBufferSize(MQTT_BUFFER_SIZE);
    _mqttClient->setCallback(handleMqttMessage);

    MqttRuntimeConfiguration candidate;
    if (loadMQTTConfig(candidate)) {
        _activeConfiguration = std::move(candidate);
        _mqttClient->setServer(_activeConfiguration.connection.broker.data(), _activeConfiguration.connection.port);
        _generation = 1U;
    } else {
        _activeConfiguration = MqttRuntimeConfiguration();
    }
    rebuildOwnedSubscriptions(false);
    updateStatusSnapshot();
    return startMqttTask();
}

bool MqttManager::loadMQTTConfig(MqttRuntimeConfiguration &candidate) const {
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

    return parseMQTTConfig(text.c_str(), text.length(), candidate);
}

bool MqttManager::parseMQTTConfig(const char *json, const size_t length,
                                  MqttRuntimeConfiguration &candidate) const {
    MqttConfigDocument::StoredConfig stored;
    if (!MqttConfigDocument::parseConfig(json, length, stored)) {
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

    MqttRuntimeConfiguration prepared;
    MqttConfigCore::ValidationError validation;
    if (!MqttConfigCore::prepareConnection(stored.connection, prepared.connection, validation)) {
        const String message = String("[MQTT] Configuration load failed: ")
                               + MqttConfigCore::fieldName(validation.field) + " "
                               + MqttConfigCore::constraintMessage(validation);
        _logger->logError(message.c_str());
        return false;
    }

    prepared.rootTopic = MqttConfigCore::trim(stored.rootTopic).c_str();
    prepared.enabled = stored.enabled;
    candidate = std::move(prepared);
    return true;
}

bool MqttManager::startMqttTask() {
    _taskRunning.store(true, std::memory_order_release);
    const BaseType_t result = xTaskCreatePinnedToCore(
        processMQTTAsync,
        "processMQTTAsync",
        MQTT_TASK_STACK,
        this,
        1,
        &_mqttTaskHandle,
        1);

    if (result != pdPASS) {
        _taskRunning.store(false, std::memory_order_release);
        _mqttTaskHandle = nullptr;
        _logger->logError("[MQTT] Failed to start owner task");
        return false;
    }
    return true;
}

MqttOperationResult MqttManager::submitAndWait(Command *command, const uint32_t waitMs,
                                               MqttTestResult *testResult,
                                               LatestRevisionCore::Admission *admission) {
    if (command == nullptr) return MqttOperationResult::Unavailable;
    if (!command->valid() || _commandQueue == nullptr || !_taskRunning.load(std::memory_order_acquire)
        || _shuttingDown.load(std::memory_order_acquire)) {
        command->release();
        return _shuttingDown.load(std::memory_order_acquire)
                   ? MqttOperationResult::Shutdown
                   : MqttOperationResult::Unavailable;
    }

    command->retain();  // owner reference; the queue transports only this pointer
    command->markQueued();
    if (xQueueSend(_commandQueue, &command, 0) != pdTRUE) {
        _commandFailureCount.fetch_add(1U, std::memory_order_relaxed);
        command->release();  // owner reference was not transferred
        command->release();  // caller reference
        return MqttOperationResult::QueueFull;
    }
    if (admission != nullptr) admission->commit();

    const bool completed = command->wait(pdMS_TO_TICKS(waitMs));
    const MqttOperationResult result = completed ? command->result : MqttOperationResult::Timeout;
    if (completed && testResult != nullptr) *testResult = command->testResult;
    if (!completed) {
        // Ordinary requests cancel while still queued. A configuration whose
        // admission was published remains eligible to finish; later admitted
        // revisions still supersede it in the owner.
        if (admission == nullptr) (void)command->cancelIfQueued();
        _commandFailureCount.fetch_add(1U, std::memory_order_relaxed);
    }
    command->release();
    return result;
}

MqttOperationResult MqttManager::publish(const MqttPublishRequest &request) {
    auto *command = new (std::nothrow) Command(Command::Type::Publish);
    if (command == nullptr) return MqttOperationResult::Unavailable;
    command->publishRequest = request;
    return submitAndWait(command);
}

MqttAdmissionResult MqttManager::submitAsync(Command *command) {
    if (command == nullptr) return MqttAdmissionResult::Unavailable;
    if (!command->valid() || _commandQueue == nullptr || !_taskRunning.load(std::memory_order_acquire)
        || _shuttingDown.load(std::memory_order_acquire)) {
        command->release();
        return _shuttingDown.load(std::memory_order_acquire)
                   ? MqttAdmissionResult::Shutdown
                   : MqttAdmissionResult::Unavailable;
    }

    command->asynchronous = true;
    command->retain();
    command->markQueued();
    if (xQueueSend(_commandQueue, &command, 0) != pdTRUE) {
        _commandFailureCount.fetch_add(1U, std::memory_order_relaxed);
        command->release();
        command->release();
        return MqttAdmissionResult::QueueFull;
    }
    command->release();
    return MqttAdmissionResult::Accepted;
}

MqttAdmissionResult MqttManager::publishAsync(const MqttPublishRequest &request,
                                              MqttPublishCallback callback) {
    auto *command = new (std::nothrow) Command(Command::Type::Publish);
    if (command == nullptr) return MqttAdmissionResult::Unavailable;
    command->publishRequest = request;
    command->publishCallback = std::move(callback);
    return submitAsync(command);
}

MqttOperationResult MqttManager::stageBridgePlan(MqttBridgePlan plan) {
    auto *command = new (std::nothrow) Command(Command::Type::ApplyBridgePlan);
    if (command == nullptr) return MqttOperationResult::Unavailable;
    command->bridgePlan = std::move(plan);
    return submitAndWait(command);
}

void MqttManager::approveBridgePlan(const uint32_t modbusGeneration) {
    _stagedBridgePlan.approve(modbusGeneration);
}

MqttOperationResult MqttManager::setEnabled(const bool enabled) {
    auto *command = new (std::nothrow) Command(Command::Type::SetEnabled);
    if (command == nullptr) return MqttOperationResult::Unavailable;
    command->enabled = enabled;
    return submitAndWait(command);
}

MqttOperationResult MqttManager::requestReconfigureFromFile() {
    MqttRuntimeConfiguration candidate;
    if (!loadMQTTConfig(candidate)) return MqttOperationResult::InvalidConfiguration;

    auto *command = new (std::nothrow) Command(Command::Type::ApplyConfiguration);
    if (command == nullptr) return MqttOperationResult::Unavailable;
    const uint32_t revision = issueConfigurationRevision();
    command->configuration = std::move(candidate);
    command->configurationRevision = revision;
    auto admission = _configurationRevisions.beginAdmission(revision);
    return submitAndWait(command, MQTT_COMMAND_WAIT_MS, nullptr, &admission);
}

MqttOperationResult MqttManager::requestReconfigure(const String &configurationJson,
                                                    const uint32_t revision) {
    if (isConfigurationRevisionObsolete(revision)) return MqttOperationResult::Superseded;
    MqttRuntimeConfiguration candidate;
    if (!parseMQTTConfig(configurationJson.c_str(), configurationJson.length(), candidate)) {
        return MqttOperationResult::InvalidConfiguration;
    }

    auto *command = new (std::nothrow) Command(Command::Type::ApplyConfiguration);
    if (command == nullptr) return MqttOperationResult::Unavailable;
    command->configuration = std::move(candidate);
    command->configurationRevision = revision;
    auto admission = _configurationRevisions.beginAdmission(revision);
    return submitAndWait(command, MQTT_COMMAND_WAIT_MS, nullptr, &admission);
}

uint32_t MqttManager::issueConfigurationRevision() {
    return _configurationRevisions.issue();
}

bool MqttManager::isConfigurationRevisionObsolete(const uint32_t revision) const {
    return _configurationRevisions.isObsolete(revision);
}

bool MqttManager::reconfigureFromFile() {
    return requestReconfigureFromFile() == MqttOperationResult::Success;
}

MqttTestResult MqttManager::testConnectOnce() {
    MqttTestResult result;
    MqttRuntimeConfiguration candidate;
    if (!loadMQTTConfig(candidate)) {
        result.operation = MqttOperationResult::InvalidConfiguration;
        return result;
    }

    auto *command = new (std::nothrow) Command(Command::Type::TestConnection);
    if (command == nullptr) return result;
    command->configuration = std::move(candidate);
    result.operation = submitAndWait(command, MQTT_COMMAND_WAIT_MS, &result);
    return result;
}

void MqttManager::processCommand(Command &command) {
    switch (command.type) {
        case Command::Type::Publish:
            executePublish(command);
            break;
        case Command::Type::ApplyConfiguration:
            applyConfiguration(std::move(command.configuration), command.configurationRevision,
                               command.result);
            break;
        case Command::Type::ApplyBridgePlan:
            stageBridgePlanOwned(std::move(command.bridgePlan), command.result);
            break;
        case Command::Type::SetEnabled:
            applyEnabled(command.enabled, command.result);
            break;
        case Command::Type::TestConnection:
            executeConnectionTest(command);
            break;
    }
    if (command.result != MqttOperationResult::Success
        && command.result != MqttOperationResult::Superseded
        && command.result != MqttOperationResult::StaleGeneration) {
        _commandFailureCount.fetch_add(1U, std::memory_order_relaxed);
    }
    if (command.type == Command::Type::Publish && command.asynchronous
        && command.publishCallback) {
        command.publishCallback(command.result);
    }
    updateStatusSnapshot();
}

void MqttManager::applyConfiguration(MqttRuntimeConfiguration configuration,
                                     const uint32_t revision,
                                     MqttOperationResult &result) {
    if (!_configurationRevisions.isLatest(revision)) {
        result = MqttOperationResult::Superseded;
        return;
    }
    if (!configuration.connection.valid) {
        result = MqttOperationResult::InvalidConfiguration;
        return;
    }

    std::vector<String> topics;
    topics.push_back(resolveTopicForRoot(MqttTopicSpec{SYSTEM_NETWORK_RESET, true}, configuration.rootTopic));
    topics.push_back(resolveTopicForRoot(MqttTopicSpec{SYSTEM_ECHO, true}, configuration.rootTopic));
    for (const auto &subscription : _bridgePlan.subscriptions) {
        const String resolved = resolveTopicForRoot(subscription.topic, configuration.rootTopic);
        if (!resolved.length() || containsTopic(topics, resolved)) {
            _logger->logError((String("[MQTT] Configuration would create duplicate subscription topic: ")
                               + resolved).c_str());
            result = MqttOperationResult::InvalidConfiguration;
            return;
        }
        topics.push_back(resolved);
    }

    disconnectOwned();
    _activeConfiguration = std::move(configuration);
    _runtimeEnabled = true;
    ++_generation;
    _mqttClient->setServer(_activeConfiguration.connection.broker.data(), _activeConfiguration.connection.port);
    rebuildOwnedSubscriptions(false);
    result = MqttOperationResult::Success;
}

void MqttManager::stageBridgePlanOwned(MqttBridgePlan plan, MqttOperationResult &result) {
    // Preserve an already approved predecessor before a later staging command
    // can replace the inactive slot.
    commitApprovedBridgePlan();

    std::vector<String> topics;
    topics.push_back(resolveTopic(MqttTopicSpec{SYSTEM_NETWORK_RESET, true}));
    topics.push_back(resolveTopic(MqttTopicSpec{SYSTEM_ECHO, true}));
    for (const auto &subscription : plan.subscriptions) {
        const String resolved = resolveTopic(subscription.topic);
        if (!resolved.length() || containsTopic(topics, resolved)) {
            _logger->logError((String("[MQTT] Rejected duplicate or empty subscription topic: ") + resolved).c_str());
            result = MqttOperationResult::InvalidConfiguration;
            return;
        }
        topics.push_back(resolved);
    }

    const uint32_t modbusGeneration = plan.modbusGeneration;
    _stagedBridgePlan.stage(std::move(plan), modbusGeneration);
    result = MqttOperationResult::Success;
}

void MqttManager::commitApprovedBridgePlan() {
    MqttBridgePlan plan;
    uint32_t approvedGeneration = 0U;
    if (!_stagedBridgePlan.takeApproved(plan, approvedGeneration)) return;

    // The will is part of MQTT CONNECT. Only an explicitly approved staged
    // plan becomes live, and it is installed as one owner-side generation.
    disconnectOwned();
    _bridgePlan = std::move(plan);
    ++_generation;
    rebuildOwnedSubscriptions(false);
    _logger->logInformation((String("[MQTT] Committed Modbus bridge generation ")
                             + String(approvedGeneration)).c_str());
    updateStatusSnapshot();
}

void MqttManager::applyEnabled(const bool enabled, MqttOperationResult &result) {
    _runtimeEnabled = enabled;
    if (!enabled) disconnectOwned();
    result = MqttOperationResult::Success;
}

void MqttManager::executePublish(Command &command) {
    const auto &request = command.publishRequest;
    if ((request.expectedMqttGeneration != 0U && request.expectedMqttGeneration != _generation)
        || (request.expectedModbusGeneration != 0U
            && request.expectedModbusGeneration != _bridgePlan.modbusGeneration)
        || (request.expectedConnectionEpoch != 0U
            && request.expectedConnectionEpoch != _connectionEpoch)) {
        command.result = MqttOperationResult::StaleGeneration;
        return;
    }
    if (!_activeConfiguration.enabled || !_runtimeEnabled) {
        command.result = MqttOperationResult::Disabled;
        return;
    }
    if (!_mqttClient->connected()) {
        command.result = MqttOperationResult::Disconnected;
        return;
    }
    command.result = _mqttClient->publish(request.topic.c_str(), request.payload.c_str(), request.retain)
                         ? MqttOperationResult::Success
                         : MqttOperationResult::BrokerFailure;
}

void MqttManager::executeConnectionTest(Command &command) {
    auto &test = command.testResult;
    const auto &candidate = command.configuration;
    test.broker = candidate.connection.broker.data();
    test.user = candidate.connection.user.data();
    if (!candidate.connection.valid) {
        command.result = test.operation = MqttOperationResult::InvalidConfiguration;
        return;
    }
    if (WiFiClass::status() != WL_CONNECTED) {
        command.result = test.operation = MqttOperationResult::Disconnected;
        return;
    }

    disconnectOwned();
    _mqttClient->setServer(candidate.connection.broker.data(), candidate.connection.port);
    const MqttWillSpec noWill;
    test.connected = connectWithConfiguration(candidate, noWill, false);
    test.clientState = _mqttClient->state();
    command.result = test.operation = test.connected
                                          ? MqttOperationResult::Success
                                          : MqttOperationResult::BrokerFailure;
    disconnectOwned();
    if (_activeConfiguration.connection.valid) {
        _mqttClient->setServer(_activeConfiguration.connection.broker.data(), _activeConfiguration.connection.port);
    }
}

bool MqttManager::connectWithConfiguration(const MqttRuntimeConfiguration &configuration,
                                           const MqttWillSpec &will,
                                           const bool subscribeAfterConnect) {
    if (!inOwnerContext()) {
        recordOwnerViolation();
        return false;
    }
    if (!configuration.connection.valid) return false;
    if (configuration.connection.broker[0] == '\0'
        || String(configuration.connection.broker.data()) == MqttConfigCore::DEFAULT_BROKER) {
        return false;
    }

    String clientId = configuration.clientId;
    if (!clientId.length()) clientId = buildDefaultClientId();
    const bool hasUser = configuration.connection.user[0] != '\0';
    bool connected = false;
    if (will.configured && will.topic.topic.length() && will.payload.length()) {
        const String willTopic = resolveTopic(will.topic);
        if (hasUser) {
            connected = _mqttClient->connect(clientId.c_str(), configuration.connection.user.data(),
                                             configuration.connection.password.data(), willTopic.c_str(), will.qos,
                                             will.retain, will.payload.c_str());
        } else {
            connected = _mqttClient->connect(clientId.c_str(), willTopic.c_str(), will.qos,
                                             will.retain, will.payload.c_str());
        }
    } else if (hasUser) {
        connected = _mqttClient->connect(clientId.c_str(), configuration.connection.user.data(),
                                         configuration.connection.password.data());
    } else {
        connected = _mqttClient->connect(clientId.c_str());
    }

    if (!connected) {
        _logger->logError((String("MQTT connect failed, rc=") + String(_mqttClient->state())).c_str());
        return false;
    }

    if (&configuration == &_activeConfiguration) {
        _activeConfiguration.clientId = clientId;
        ++_connectionEpoch;
    }
    IndicatorService::instance().setMqttConnected(true);
    if (subscribeAfterConnect) {
        for (const auto &topic : _subscriptionHandler->getHandlerTopics()) {
            if (!_mqttClient->subscribe(topic.c_str())) {
                _commandFailureCount.fetch_add(1U, std::memory_order_relaxed);
                _logger->logWarning((String("[MQTT] Subscribe failed: ") + topic).c_str());
            }
        }
    }
    return true;
}

bool MqttManager::ensureMQTTConnection() {
    if (!inOwnerContext()) {
        recordOwnerViolation();
        return false;
    }
    return connectWithConfiguration(_activeConfiguration, _bridgePlan.will, true);
}

void MqttManager::disconnectOwned() {
    if (!inOwnerContext()) {
        recordOwnerViolation();
        return;
    }
    if (_mqttClient->connected()) _mqttClient->disconnect();
    IndicatorService::instance().setMqttConnected(false);
}

String MqttManager::resolveTopic(const MqttTopicSpec &topic) const {
    return resolveTopicForRoot(topic, _activeConfiguration.rootTopic);
}

String MqttManager::resolveTopicForRoot(const MqttTopicSpec &topic, const String &rootTopic) {
    String resolved = topic.topic;
    resolved.trim();
    if (!topic.relativeToRoot) return resolved;

    String root = rootTopic;
    root.trim();
    while (resolved.startsWith("/")) resolved.remove(0, 1);
    if (!root.length()) return resolved;
    if (!root.endsWith("/")) root += "/";
    return root + resolved;
}

std::vector<MqttSubscriptionHandler::HandlerEntry> MqttManager::buildHandlerEntries() const {
    std::vector<MqttSubscriptionHandler::HandlerEntry> handlers;
    handlers.reserve(_bridgePlan.subscriptions.size() + 2U);
    handlers.push_back({resolveTopic(MqttTopicSpec{SYSTEM_NETWORK_RESET, true}), [this](const String &) {
                            _logger->logInformation("[MQTT][Subscriptions] Network reset requested");
                        }});
    handlers.push_back({resolveTopic(MqttTopicSpec{SYSTEM_ECHO, true}), [this](const String &message) {
                            _logger->logInformation("[MQTT][Subscriptions] Echo requested");
                            _logger->logInformation(message.c_str());
                        }});
    for (const auto &subscription : _bridgePlan.subscriptions) {
        handlers.push_back({resolveTopic(subscription.topic), subscription.handler});
    }
    return handlers;
}

void MqttManager::rebuildOwnedSubscriptions(const bool reconcileBroker) {
    if (!inOwnerContext()) {
        recordOwnerViolation();
        return;
    }

    const std::vector<String> oldTopics = _subscriptionHandler->getHandlerTopics();
    auto handlers = buildHandlerEntries();
    std::vector<String> newTopics;
    newTopics.reserve(handlers.size());
    for (const auto &handler : handlers) newTopics.push_back(handler.topic);
    _subscriptionHandler->replaceHandlers(std::move(handlers));

    if (!reconcileBroker || !_mqttClient->connected()) return;
    for (const auto &topic : oldTopics) {
        if (!containsTopic(newTopics, topic)) _mqttClient->unsubscribe(topic.c_str());
    }
    for (const auto &topic : newTopics) {
        if (!containsTopic(oldTopics, topic)) _mqttClient->subscribe(topic.c_str());
    }
}

void MqttManager::handleMqttMessage(char *topic, const byte *payload, const unsigned int length) {
    if (s_activeMqttManager != nullptr) {
        s_activeMqttManager->onMqttMessage(String(topic), payload, length);
    }
}

void MqttManager::onMqttMessage(const String &topic, const uint8_t *payload, const size_t length) const {
    String message;
    message.reserve(length);
    for (size_t i = 0; i < length; ++i) message += static_cast<char>(payload[i]);
    _subscriptionHandler->handle(topic, message);
}

void MqttManager::updateStatusSnapshot() {
    if (_statusMutex == nullptr) return;
    MqttStatusSnapshot snapshot;
    snapshot.available = true;
    snapshot.enabled = _activeConfiguration.enabled && _runtimeEnabled;
    snapshot.connected = _mqttClient != nullptr && _mqttClient->connected();
    snapshot.clientState = _mqttClient != nullptr ? _mqttClient->state() : -1;
    snapshot.generation = _generation;
    snapshot.connectionEpoch = _connectionEpoch;
    snapshot.modbusGeneration = _bridgePlan.modbusGeneration;
    snapshot.ownerViolationCount = _ownerViolationCount.load(std::memory_order_relaxed);
    snapshot.commandFailureCount = _commandFailureCount.load(std::memory_order_relaxed);
    snapshot.broker = _activeConfiguration.connection.broker.data();
    snapshot.user = _activeConfiguration.connection.user.data();
    snapshot.rootTopic = _activeConfiguration.rootTopic;
    snapshot.clientId = _activeConfiguration.clientId;

    if (xSemaphoreTake(_statusMutex, pdMS_TO_TICKS(25)) == pdTRUE) {
        _statusSnapshot = std::move(snapshot);
        xSemaphoreGive(_statusMutex);
    }
}

MqttStatusSnapshot MqttManager::getStatusSnapshot() const {
    MqttStatusSnapshot snapshot;
    if (_statusMutex != nullptr && xSemaphoreTake(_statusMutex, pdMS_TO_TICKS(25)) == pdTRUE) {
        snapshot = _statusSnapshot;
        xSemaphoreGive(_statusMutex);
    }
    snapshot.ownerViolationCount = _ownerViolationCount.load(std::memory_order_relaxed);
    snapshot.commandFailureCount = _commandFailureCount.load(std::memory_order_relaxed);
    return snapshot;
}

bool MqttManager::isConnected() const {
    return getStatusSnapshot().connected;
}

bool MqttManager::inOwnerContext() const {
    if (!_taskRunning.load(std::memory_order_acquire) || _mqttTaskHandle == nullptr) return true;
    return xTaskGetCurrentTaskHandle() == _mqttTaskHandle;
}

void MqttManager::recordOwnerViolation() {
    _ownerViolationCount.fetch_add(1U, std::memory_order_relaxed);
    _logger->logError("[MQTT] Owner-context violation blocked");
}

void MqttManager::setMQTTEnabled(const bool enabled) {
    if (s_activeMqttManager != nullptr) (void)s_activeMqttManager->setEnabled(enabled);
}

bool MqttManager::isMQTTEnabled() {
    return s_activeMqttManager != nullptr && s_activeMqttManager->getStatusSnapshot().enabled;
}

[[noreturn]] void MqttManager::processMQTTAsync(void *parameter) {
    auto *manager = static_cast<MqttManager *>(parameter);
    constexpr TickType_t delayTicks = pdMS_TO_TICKS(MQTT_TASK_LOOP_DELAY_MS);
    unsigned long lastReconnectAttempt = 0U;

    while (true) {
        for (size_t i = 0; i < MQTT_COMMANDS_PER_CYCLE; ++i) {
            Command *command = nullptr;
            if (xQueueReceive(manager->_commandQueue, &command, 0) != pdTRUE) break;
            if (command != nullptr && command->tryStart()) {
                manager->processCommand(*command);
                command->complete();
            }
            if (command != nullptr) command->release();
        }

        manager->commitApprovedBridgePlan();

        const bool enabled = manager->_activeConfiguration.enabled && manager->_runtimeEnabled;
        const bool wifiConnected = WiFiClass::status() == WL_CONNECTED;
        if (!enabled || !wifiConnected) {
            manager->disconnectOwned();
            manager->updateStatusSnapshot();
            vTaskDelay(delayTicks);
            continue;
        }

        if (!manager->_mqttClient->connected()) {
            const unsigned long now = millis();
            if (now - lastReconnectAttempt >= MQTT_RECONNECT_INTERVAL_MS) {
                lastReconnectAttempt = now;
                (void)manager->ensureMQTTConnection();
            }
        } else {
            (void)manager->_mqttClient->loop();
        }
        IndicatorService::instance().setMqttConnected(manager->_mqttClient->connected());
        manager->updateStatusSnapshot();
        vTaskDelay(delayTicks);
    }
}
