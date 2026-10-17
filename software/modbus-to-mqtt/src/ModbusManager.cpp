#include "modbus/ModbusManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <string>
#include <utility>

#include "concurrency/OwnerRequest.h"
#include "modbus/ModbusConfigLoader.h"
#include "modbus/ModbusFunctionUtils.h"
#include "modbus/ModbusMqttWriteCore.h"
#include "modbus/ModbusPollScheduler.h"
#include "modbus/ModbusValueCodec.h"
#include "modbus/WavinClient.h"
#include "modbus/WavinFrame.h"
#include "mqtt/MqttManager.h"
#include "services/IndicatorService.h"
#include "storage/ConfigFs.h"

namespace {

constexpr UBaseType_t OTA_CONTROL_QUEUE_DEPTH = 2U;

// Where a datapoint lives, for log lines: the flat address, or the register
// index with its category and page for Wavin functions.
String describeAddress(const ModbusFunctionType function, const uint16_t address,
                       const uint8_t category, const uint8_t page) {
    if (!isWavinFunction(function)) return String("addr=") + String(address);
    return String("category=") + String(category) + ", index=" + String(address)
           + ", page=" + String(page);
}

}

struct ModbusManager::Command final : OwnerRequest {
    enum class Type : uint8_t {
        ActivateConfiguration,
        Execute,
        SetEnabled,
        PublicationCompletion,
        QuiesceForOta,
        ResumeFromOta,
    };

    explicit Command(const Type commandType)
        : type(commandType) {
    }

    Type type;
    ConfigurationRoot configuration;
    ModbusCommandRequest request;
    ModbusCommandResult commandResult;
    ModbusPublicationCompletion publicationCompletion;
    ModbusOperationResult result{ModbusOperationResult::Unavailable};
    uint32_t configurationRevision{0U};
    bool enabled{false};
    bool asynchronous{false};
    uint32_t otaOperationId{0U};
};

ModbusManager::ModbusManager(Logger *logger)
    : _bus(logger),
      _mqttBridge(logger, this),
      _logger(logger) {
}

bool ModbusManager::begin() {
    _ownerTaskHandle = xTaskGetCurrentTaskHandle();
    _commandQueue = xQueueCreate(MODBUS_COMMAND_QUEUE_DEPTH, sizeof(Command *));
    _controlQueue = xQueueCreate(OTA_CONTROL_QUEUE_DEPTH, sizeof(Command *));
    if (_commandQueue == nullptr || _controlQueue == nullptr) {
        _logger->logError("[Modbus] Failed to initialize owner mailbox");
        return false;
    }
    _ownerAvailable.store(true, std::memory_order_release);

    ConfigurationRoot candidate;
    if (!loadConfigurationCandidate(candidate)) {
        _bus.setActive(false);
        updateRuntimeSnapshot();
        return false;
    }
    const ModbusOperationResult activation = activateConfiguration(std::move(candidate));
    if (activation != ModbusOperationResult::Success) {
        _bus.setActive(false);
        updateRuntimeSnapshot();
        _logger->logError((String("[Modbus] Initial activation failed: ")
                           + modbusOperationResultToString(activation)).c_str());
        return false;
    }
    return _bus.isActive();
}

bool ModbusManager::loadConfigurationCandidate(ConfigurationRoot &candidate) const {
    return ModbusConfigLoader::loadConfiguration(_logger, ConfigFs::kModbusConfigFile, candidate);
}

ModbusOperationResult ModbusManager::activateConfiguration(ConfigurationRoot candidate,
                                                           const uint32_t revision) {
    if (!inOwnerContext()) {
        recordOwnerViolation();
        return ModbusOperationResult::Unavailable;
    }

    if (revision != 0U && !_configurationRevisions.isLatest(revision)) {
        return ModbusOperationResult::Superseded;
    }

    const uint32_t nextGeneration = _generation + 1U;
    const MqttOperationResult mqttResult = _mqttBridge.stageConfiguration(candidate, nextGeneration);
    if (mqttResult != MqttOperationResult::Success) {
        _logger->logError((String("[Modbus] MQTT bridge plan rejected: ")
                           + mqttOperationResultToString(mqttResult)).c_str());
        return ModbusOperationResult::ActivationFailed;
    }
    if (revision != 0U && !_configurationRevisions.isLatest(revision)) {
        return ModbusOperationResult::Superseded;
    }

    // The owner has finished every prior polling pass/command before reaching
    // this method. Wiring initialization and the root swap therefore form one
    // exclusive transition and invalidate no live iterator or bus user.
    if (!_bus.begin(candidate.bus)) return ModbusOperationResult::ActivationFailed;
    _modbusRoot = std::move(candidate);
    _generation = nextGeneration;
    _mqttBridge.onConfigurationActivated(_modbusRoot);
    _bus.setActive(_modbusRoot.bus.enabled);
    _mqttBridge.commitConfiguration(_generation);
    updateRuntimeSnapshot();
    _logger->logInformation((String("[Modbus] Activated generation ") + String(_generation)
                             + " with " + String(_modbusRoot.devices.size()) + " devices").c_str());
    return ModbusOperationResult::Success;
}

ModbusOperationResult ModbusManager::submitAndWait(Command *command, const uint32_t waitMs,
                                                   ModbusCommandResult *result,
                                                   LatestRevisionCore::Admission *admission) {
    if (command == nullptr) return ModbusOperationResult::Unavailable;
    if (!_ordinaryAdmissionOpen.load(std::memory_order_acquire)) {
        command->release();
        return ModbusOperationResult::OtaInProgress;
    }
    if (!command->valid() || _commandQueue == nullptr || !_ownerAvailable.load(std::memory_order_acquire)
        || _shuttingDown.load(std::memory_order_acquire)) {
        command->release();
        return _shuttingDown.load(std::memory_order_acquire)
                   ? ModbusOperationResult::Shutdown
                   : ModbusOperationResult::Unavailable;
    }

    command->retain();
    command->markQueued();
    if (xQueueSend(_commandQueue, &command, 0) != pdTRUE) {
        _commandFailureCount.fetch_add(1U, std::memory_order_relaxed);
        command->release();
        command->release();
        return ModbusOperationResult::QueueFull;
    }
    if (admission != nullptr) admission->commit();

    const bool completed = command->wait(pdMS_TO_TICKS(waitMs));
    const ModbusOperationResult operation = completed ? command->result : ModbusOperationResult::Timeout;
    if (completed && result != nullptr) *result = command->commandResult;
    if (!completed) {
        // Configuration admission is already published once its owner command
        // is queued. Leave that command eligible to finish after a caller
        // timeout; a later admitted revision will still supersede it.
        if (admission == nullptr) (void)command->cancelIfQueued();
        _commandFailureCount.fetch_add(1U, std::memory_order_relaxed);
    }
    command->release();
    return operation;
}

ModbusOperationResult ModbusManager::submitAsync(Command *command) {
    if (command == nullptr) return ModbusOperationResult::Unavailable;
    if (!_ordinaryAdmissionOpen.load(std::memory_order_acquire)) {
        command->release();
        return ModbusOperationResult::OtaInProgress;
    }
    if (!command->valid() || _commandQueue == nullptr || !_ownerAvailable.load(std::memory_order_acquire)
        || _shuttingDown.load(std::memory_order_acquire)) {
        command->release();
        return ModbusOperationResult::Unavailable;
    }

    command->asynchronous = true;
    command->retain();
    command->markQueued();
    if (xQueueSend(_commandQueue, &command, 0) != pdTRUE) {
        _commandFailureCount.fetch_add(1U, std::memory_order_relaxed);
        command->release();
        command->release();
        return ModbusOperationResult::QueueFull;
    }
    command->release();
    return ModbusOperationResult::Success;
}

ModbusOperationResult ModbusManager::submitControl(Command *command) {
    if (command == nullptr) return ModbusOperationResult::Unavailable;
    if (!command->valid() || _controlQueue == nullptr || !_ownerAvailable.load(std::memory_order_acquire)
        || _shuttingDown.load(std::memory_order_acquire)) {
        command->release();
        return _shuttingDown.load(std::memory_order_acquire)
                   ? ModbusOperationResult::Shutdown
                   : ModbusOperationResult::Unavailable;
    }
    command->asynchronous = true;
    command->retain();
    command->markQueued();
    if (xQueueSend(_controlQueue, &command, 0) != pdTRUE) {
        _commandFailureCount.fetch_add(1U, std::memory_order_relaxed);
        command->release();
        command->release();
        return ModbusOperationResult::QueueFull;
    }
    command->release();
    return ModbusOperationResult::Success;
}

ModbusCommandResult ModbusManager::executeCommand(const ModbusCommandRequest &request) {
    ModbusCommandResult result;
    auto *command = new (std::nothrow) Command(Command::Type::Execute);
    if (command == nullptr) return result;
    command->request = request;
    result.operation = submitAndWait(command, MODBUS_COMMAND_WAIT_MS, &result);
    return result;
}

ModbusOperationResult ModbusManager::submitWriteCommand(const ModbusCommandRequest &request) {
    auto *command = new (std::nothrow) Command(Command::Type::Execute);
    if (command == nullptr) return ModbusOperationResult::Unavailable;
    command->request = request;
    return submitAsync(command);
}

ModbusOperationResult ModbusManager::requestReconfigureFromFile() {
    ConfigurationRoot candidate;
    if (!loadConfigurationCandidate(candidate)) return ModbusOperationResult::ActivationFailed;

    auto *command = new (std::nothrow) Command(Command::Type::ActivateConfiguration);
    if (command == nullptr) return ModbusOperationResult::Unavailable;
    const uint32_t revision = issueConfigurationRevision();
    command->configuration = std::move(candidate);
    command->configurationRevision = revision;
    auto admission = _configurationRevisions.beginAdmission(revision);
    return submitAndWait(command, MODBUS_COMMAND_WAIT_MS, nullptr, &admission);
}

ModbusOperationResult ModbusManager::requestReconfigure(const String &configurationJson,
                                                        const uint32_t revision) {
    if (isConfigurationRevisionObsolete(revision)) return ModbusOperationResult::Superseded;
    ConfigurationRoot candidate;
    if (!ModbusConfigLoader::parseConfiguration(_logger, configurationJson.c_str(),
                                                configurationJson.length(), candidate)) {
        return ModbusOperationResult::ActivationFailed;
    }

    auto *command = new (std::nothrow) Command(Command::Type::ActivateConfiguration);
    if (command == nullptr) return ModbusOperationResult::Unavailable;
    command->configuration = std::move(candidate);
    command->configurationRevision = revision;
    auto admission = _configurationRevisions.beginAdmission(revision);
    return submitAndWait(command, MODBUS_COMMAND_WAIT_MS, nullptr, &admission);
}

uint32_t ModbusManager::issueConfigurationRevision() {
    return _configurationRevisions.issue();
}

bool ModbusManager::isConfigurationRevisionObsolete(const uint32_t revision) const {
    return _configurationRevisions.isObsolete(revision);
}

bool ModbusManager::reconfigureFromFile() {
    return requestReconfigureFromFile() == ModbusOperationResult::Success;
}

ModbusOperationResult ModbusManager::setEnabled(const bool enabled) {
    auto *command = new (std::nothrow) Command(Command::Type::SetEnabled);
    if (command == nullptr) return ModbusOperationResult::Unavailable;
    command->enabled = enabled;
    return submitAndWait(command);
}

ModbusOperationResult ModbusManager::requestQuiesceForOta(const uint32_t operationId) {
    if (operationId == 0U) return ModbusOperationResult::Unavailable;
    uint32_t expected = 0U;
    if (!_otaAdmissionOperation.compare_exchange_strong(
            expected, operationId, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return expected == operationId ? ModbusOperationResult::Success
                                       : ModbusOperationResult::OtaInProgress;
    }
    _ordinaryAdmissionOpen.store(false, std::memory_order_release);
    auto *command = new (std::nothrow) Command(Command::Type::QuiesceForOta);
    if (command == nullptr) {
        _otaAdmissionOperation.store(0U, std::memory_order_release);
        _ordinaryAdmissionOpen.store(true, std::memory_order_release);
        return ModbusOperationResult::Unavailable;
    }
    command->otaOperationId = operationId;
    const ModbusOperationResult result = submitControl(command);
    if (result != ModbusOperationResult::Success) {
        _otaAdmissionOperation.store(0U, std::memory_order_release);
        _ordinaryAdmissionOpen.store(true, std::memory_order_release);
    }
    return result;
}

ModbusOperationResult ModbusManager::requestResumeFromOta(const uint32_t operationId) {
    if (operationId == 0U
        || _otaAdmissionOperation.load(std::memory_order_acquire) != operationId) {
        return ModbusOperationResult::Unavailable;
    }
    auto *command = new (std::nothrow) Command(Command::Type::ResumeFromOta);
    if (command == nullptr) return ModbusOperationResult::Unavailable;
    command->otaOperationId = operationId;
    return submitControl(command);
}

void ModbusManager::cancelOtaQuiesceRequest(const uint32_t operationId) {
    (void)requestResumeFromOta(operationId);
}

bool ModbusManager::otaControlActive() const {
    return _otaAdmissionOperation.load(std::memory_order_acquire) != 0U;
}

ModbusOperationResult ModbusManager::submitPublicationCompletion(
    const ModbusPublicationCompletion &completion) {
    auto *command = new (std::nothrow) Command(Command::Type::PublicationCompletion);
    if (command == nullptr) return ModbusOperationResult::Unavailable;
    command->publicationCompletion = completion;
    return submitAsync(command);
}

void ModbusManager::drainCommands() {
    for (size_t i = 0; i < OTA_CONTROL_QUEUE_DEPTH; ++i) {
        Command *command = nullptr;
        if (xQueueReceive(_controlQueue, &command, 0) != pdTRUE) break;
        if (command != nullptr && command->tryStart()) {
            processCommand(*command);
            command->complete();
        }
        if (command != nullptr) command->release();
    }
    for (size_t i = 0; i < MODBUS_COMMANDS_PER_CYCLE; ++i) {
        // A control command that arrived while the previous command ran gets
        // the next loop() call. The rest of this batch would otherwise hold
        // the bus for one transaction each (a Wavin write with its retries
        // takes about two seconds) before the quiesce could reject it.
        if (controlPending()) break;
        Command *command = nullptr;
        if (xQueueReceive(_commandQueue, &command, 0) != pdTRUE) break;
        if (command != nullptr && command->tryStart()) {
            processCommand(*command);
            command->complete();
        }
        if (command != nullptr) command->release();
    }
}

bool ModbusManager::controlPending() const {
    if (_controlQueue != nullptr && uxQueueMessagesWaiting(_controlQueue) > 0U) return true;
    // requestQuiesceForOta() closes ordinary admission before it posts the
    // quiesce command, so a closed admission announces a control command
    // until the owner has quiesced. It then stays closed until the resume,
    // while the ordinary commands still queued are drained (and rejected).
    return !_otaQuiesced && !_ordinaryAdmissionOpen.load(std::memory_order_acquire);
}

void ModbusManager::processCommand(Command &command) {
    if (_otaQuiesced && command.type != Command::Type::QuiesceForOta
        && command.type != Command::Type::ResumeFromOta) {
        command.result = ModbusOperationResult::OtaInProgress;
        if (command.type == Command::Type::Execute) {
            command.commandResult.operation = ModbusOperationResult::OtaInProgress;
        }
    } else {
    switch (command.type) {
        case Command::Type::ActivateConfiguration:
            command.result = activateConfiguration(std::move(command.configuration),
                                                   command.configurationRevision);
            break;
        case Command::Type::Execute:
            command.commandResult = executeOwned(command.request);
            command.result = command.commandResult.operation;
            if (command.asynchronous) {
                const String prefix = command.request.sourceTopic.length()
                                          ? String("[Modbus][MQTT] ") + command.request.sourceTopic + ": "
                                          : String("[Modbus][Async] ");
                if (command.commandResult.operation == ModbusOperationResult::Success
                    && command.commandResult.busStatus == ModbusMaster::ku8MBSuccess) {
                    _logger->logDebug((prefix + "command completed").c_str());
                } else {
                    _logger->logError((prefix + modbusOperationResultToString(command.commandResult.operation)
                                       + ", bus=" + statusToString(command.commandResult.busStatus)).c_str());
                }
            }
            break;
        case Command::Type::SetEnabled:
            _bus.setActive(command.enabled);
            command.result = ModbusOperationResult::Success;
            break;
        case Command::Type::PublicationCompletion:
            _mqttBridge.onPublicationCompletion(_modbusRoot, command.publicationCompletion,
                                               _generation);
            command.result = ModbusOperationResult::Success;
            break;
        case Command::Type::QuiesceForOta:
            quiesceForOtaOwned(command.otaOperationId, command.result);
            break;
        case Command::Type::ResumeFromOta:
            resumeFromOtaOwned(command.otaOperationId, command.result);
            break;
    }
    }
    if (command.result != ModbusOperationResult::Success
        && command.result != ModbusOperationResult::Superseded
        && command.result != ModbusOperationResult::StaleGeneration) {
        _commandFailureCount.fetch_add(1U, std::memory_order_relaxed);
    }
    updateRuntimeSnapshot();
}

void ModbusManager::quiesceForOtaOwned(const uint32_t operationId,
                                       ModbusOperationResult &result) {
    if (operationId == 0U) {
        result = ModbusOperationResult::Unavailable;
        return;
    }
    if (_otaAdmissionOperation.load(std::memory_order_acquire) != operationId) {
        result = ModbusOperationResult::StaleGeneration;
        return;
    }
    if (_otaQuiesced) {
        result = operationId == _otaOperationId
                     ? ModbusOperationResult::Success
                     : ModbusOperationResult::OtaInProgress;
        return;
    }
    _busActiveBeforeOta = _bus.isActive();
    _otaQuiesceResult = _busActiveBeforeOta ? ModbusOtaQuiesceResult::Stopped
                                            : ModbusOtaQuiesceResult::AlreadyDisabled;
    _bus.setActive(false);
    _otaOperationId = operationId;
    _otaQuiesced = true;
    result = ModbusOperationResult::Success;

    // Reject all bounded work overtaken by the priority quiesce command so it
    // cannot execute after a later recovery resume.
    Command *queued = nullptr;
    while (xQueueReceive(_commandQueue, &queued, 0) == pdTRUE) {
        if (queued != nullptr && queued->tryStart()) {
            processCommand(*queued);
            queued->complete();
        }
        if (queued != nullptr) queued->release();
        queued = nullptr;
    }
}

void ModbusManager::resumeFromOtaOwned(const uint32_t operationId,
                                      ModbusOperationResult &result) {
    if (operationId == 0U
        || _otaAdmissionOperation.load(std::memory_order_acquire) != operationId) {
        result = ModbusOperationResult::StaleGeneration;
        return;
    }
    if (_otaQuiesced) {
        if (operationId != _otaOperationId) {
            result = ModbusOperationResult::StaleGeneration;
            return;
        }
        _bus.setActive(_busActiveBeforeOta);
        _otaQuiesced = false;
        _otaOperationId = 0U;
        _otaQuiesceResult = ModbusOtaQuiesceResult::None;
    }
    _otaAdmissionOperation.store(0U, std::memory_order_release);
    _ordinaryAdmissionOpen.store(true, std::memory_order_release);
    result = ModbusOperationResult::Success;
}

void ModbusManager::loop() {
    if (!inOwnerContext()) {
        recordOwnerViolation();
        return;
    }

    // This is the activation/control boundary. drainCommands() stops between
    // queued commands once a control command is pending, and polling is
    // skipped while one is, so after a priority OTA command arrives at most
    // one transaction (with its retries) runs before the command is observed:
    // the one in progress, whether a poll or a queued command. The control
    // command is processed first on the next loop() call.
    drainCommands();
    if (controlPending()) {
        updateRuntimeSnapshot();
        return;
    }

    if (!_bus.isActive()) {
        IndicatorService::instance().setModbusConnected(false);
        updateRuntimeSnapshot();
        return;
    }

    _mqttBridge.recoverLostPublicationCompletions(_modbusRoot);

    if (_mqtt != nullptr) {
        const MqttStatusSnapshot mqttStatus = _mqtt->getStatusSnapshot();
        _mqttBridge.onConnectionState(mqttStatus, _modbusRoot, _generation);
    }

    bool anySuccess = false;
    bool anyAttempted = false;
    const uint32_t now = millis();
    for (auto &device : _modbusRoot.devices) {
        _dueScratch.clear();
        const size_t dueCount = ModbusPollScheduler::collectDueReadDatapoints(device, now, _dueScratch);
        if (dueCount == 0U) continue;
        anyAttempted = true;
        anySuccess = readModbusDevice(device, _dueScratch, now) || anySuccess;
        break;
    }
    if (anyAttempted) IndicatorService::instance().setModbusConnected(anySuccess);
    updateRuntimeSnapshot();
}

bool ModbusManager::readModbusDevice(ModbusDevice &device,
                                     const std::vector<ModbusDatapoint *> &dueDatapoints,
                                     const uint32_t now) {
    auto guard = _bus.port().acquire();
    if (!guard) return false;

    ModbusMaster &node = _bus.node();
    node.begin(device.slaveId, _bus.port().stream());
    bool successOnDevice = false;
    for (auto *datapointPointer : dueDatapoints) {
        if (datapointPointer == nullptr) continue;
        auto &datapoint = *datapointPointer;
        uint8_t wordCount = datapoint.numOfRegisters ? datapoint.numOfRegisters : 1U;
        std::vector<uint16_t> words(wordCount);
        uint8_t result = ModbusMaster::ku8MBIllegalFunction;
        switch (datapoint.function) {
            case READ_COIL: result = node.readCoils(datapoint.address, datapoint.numOfRegisters); break;
            case READ_DISCRETE: result = node.readDiscreteInputs(datapoint.address, datapoint.numOfRegisters); break;
            case READ_HOLDING: result = node.readHoldingRegisters(datapoint.address, datapoint.numOfRegisters); break;
            case READ_INPUT: result = node.readInputRegisters(datapoint.address, datapoint.numOfRegisters); break;
            case WAVIN_READ: {
                // The controller may answer with fewer registers than asked
                // for; only those received are decoded.
                uint8_t received = 0U;
                result = WavinClient(_bus.port()).readRegisters(device.slaveId, datapoint.category,
                                                         static_cast<uint8_t>(datapoint.address),
                                                         datapoint.page, wordCount, words.data(),
                                                         words.size(), received);
                if (result == ModbusMaster::ku8MBSuccess) wordCount = received;
                break;
            }
            case WRITE_COIL:
            case WRITE_HOLDING:
            case WRITE_MULTIPLE_HOLDING:
            case WAVIN_WRITE:
                ModbusPollScheduler::scheduleNext(datapoint, now);
                continue;
        }

        if (result == ModbusMaster::ku8MBSuccess) {
            successOnDevice = true;
            if (!isWavinFunction(datapoint.function)) {
                for (uint8_t i = 0; i < wordCount; ++i) words[i] = node.getResponseBuffer(i);
            }
            String payload;
            bool publish = true;
            if (datapoint.dataType == TEXT) {
                payload = registersToAscii(words.data(), wordCount);
            } else {
                // Decoded by the configured data type before the scale is
                // applied, with the same sequence the HTTP execute route
                // uses, so both produce identical text.
                char text[64];
                const ModbusValueCodec::ReadingText reading = ModbusValueCodec::formatReading(
                    words.data(), wordCount, isBitFunction(datapoint.function), datapoint.dataType,
                    datapoint.registerSlice, datapoint.wordOrder, datapoint.scale, text, sizeof text);
                if (reading.ok) {
                    payload = text;
                } else {
                    publish = false;
                    _logger->logDebug((String("Modbus value skipped - ") + device.name + ": "
                                       + describeAddress(datapoint.function, datapoint.address,
                                                         datapoint.category, datapoint.page)
                                       + " needs "
                                       + String(reading.width)
                                       + " registers, read " + String(wordCount)).c_str());
                }
            }
            if (publish) _mqttBridge.publishDatapoint(device, datapoint, payload, _generation);
        } else {
            incrementBusErrorCount();
            _logger->logError((String("Modbus ERR - ") + device.name + ": func="
                               + functionToString(datapoint.function) + ", "
                               + describeAddress(datapoint.function, datapoint.address,
                                                 datapoint.category, datapoint.page)
                               + ", code=" + String(result) + " (" + statusToString(result) + ")"
                               + _bus.port().dumpRx()).c_str());
        }
        ModbusPollScheduler::scheduleNext(datapoint, now);
        return successOnDevice;
    }
    return successOnDevice;
}

ModbusCommandResult ModbusManager::executeOwned(const ModbusCommandRequest &request) {
    ModbusCommandResult result;
    result.generation = _generation;
    if (!inOwnerContext()) {
        recordOwnerViolation();
        return result;
    }
    if (request.expectedGeneration != 0U && request.expectedGeneration != _generation) {
        result.operation = ModbusOperationResult::StaleGeneration;
        return result;
    }

    // The datapoint the request names, looked up within its device so a
    // datapoint id reused on another device cannot lend its encoding. A
    // device id the configuration does not contain falls back to the first
    // datapoint with that id, for callers that only know the datapoint.
    const ModbusDevice *metadataDevice = nullptr;
    const ModbusDatapoint *metadata = nullptr;
    for (const auto &device : _modbusRoot.devices) {
        if (device.id != request.deviceId) continue;
        metadataDevice = &device;
        for (const auto &datapoint : device.datapoints) {
            if (datapoint.id == request.datapointId) {
                metadata = &datapoint;
                break;
            }
        }
        break;
    }
    if (metadataDevice == nullptr) {
        for (const auto &device : _modbusRoot.devices) {
            for (const auto &datapoint : device.datapoints) {
                if (datapoint.id == request.datapointId) {
                    metadata = &datapoint;
                    metadataDevice = &device;
                    break;
                }
            }
            if (metadata != nullptr) break;
        }
    }
    if (metadata != nullptr) {
        result.hasDatapointMetadata = true;
        result.dataType = metadata->dataType;
        result.registerSlice = metadata->registerSlice;
        result.wordOrder = metadata->wordOrder;
        result.scale = metadata->scale;
    }

    result.slaveId = request.hasSlaveOverride ? request.slaveId
                     : metadataDevice != nullptr ? metadataDevice->slaveId
                                                 : MODBUS_SLAVE_ID;
    const bool wavin = isWavinFunctionCode(request.function);
    const bool registerWrite = request.function == 6 || request.function == 16
                               || request.function == WAVIN_WRITE;
    const bool expectedWrite = request.function == 5 || registerWrite;
    const bool expectedRead = (request.function >= 1 && request.function <= 4)
                              || request.function == WAVIN_READ;
    if ((expectedWrite && !request.hasWriteValue) || (!expectedRead && !expectedWrite)) {
        result.operation = ModbusOperationResult::Success;
        result.busStatus = expectedWrite
                               ? ModbusMaster::ku8MBIllegalDataValue
                               : ModbusMaster::ku8MBIllegalFunction;
        return result;
    }

    bool coilOn = request.writeValue != 0U;
    if (request.function == 5 && request.writeText.length() > 0U) {
        // HTTP path: the coil state arrives as text and follows the MQTT
        // command grammar (0/1/false/true). Anything else is rejected
        // instead of being read as 0 and switching the coil off.
        const ModbusMqttWriteCore::ConversionResult conversion = ModbusMqttWriteCore::convertPayload(
            WRITE_COIL, std::string(request.writeText.c_str()), 1.0);
        if (!conversion.accepted()) {
            result.operation = ModbusOperationResult::Success;
            result.busStatus = ModbusMaster::ku8MBIllegalDataValue;
            result.writeRejection = conversion.rejection;
            return result;
        }
        coilOn = conversion.rawValue != 0U;
    }

    // Wavin registers sit at category/index/page; the request names them or
    // inherits them from the saved datapoint.
    uint8_t wavinCategory = request.category;
    uint8_t wavinPage = request.page;
    if (wavin && !request.hasWavinAddress) {
        if (metadata == nullptr) {
            // Without a saved datapoint there is no category or page to
            // inherit; defaulting both to 0 would address another register.
            result.operation = ModbusOperationResult::Success;
            result.busStatus = ModbusMaster::ku8MBIllegalDataAddress;
            result.unknownDatapoint = true;
            return result;
        }
        wavinCategory = metadata->category;
        wavinPage = metadata->page;
    }
    if (wavin && request.address > 0xFFU) {
        result.operation = ModbusOperationResult::Success;
        result.busStatus = ModbusMaster::ku8MBIllegalDataAddress;
        return result;
    }
    if (request.function == WAVIN_READ
        && (request.length == 0U || request.length > WavinFrame::kMaxRegisters)) {
        result.operation = ModbusOperationResult::Success;
        result.busStatus = ModbusMaster::ku8MBIllegalDataValue;
        return result;
    }

    // Register writes carry pre-encoded words (MQTT path) or an engineering
    // value to encode with the saved datapoint's data type, scale and word
    // order (HTTP path). A value that cannot be encoded never reaches the bus.
    std::array<uint16_t, WavinFrame::kMaxRegisters> words = request.writeWords;
    uint8_t wordCount = request.writeCount;
    if (registerWrite) {
        if (wordCount == 0U) {
            if (metadata == nullptr) {
                // Without a saved datapoint there is no data type or scale
                // to encode with; writing the text as a raw uint16 would set
                // the wrong value (and only the first register of a wide type).
                result.operation = ModbusOperationResult::Success;
                result.busStatus = ModbusMaster::ku8MBIllegalDataValue;
                result.unknownDatapoint = true;
                return result;
            }
            const ModbusFunctionType function = request.function == 6    ? WRITE_HOLDING
                                                : request.function == 16 ? WRITE_MULTIPLE_HOLDING
                                                                         : WAVIN_WRITE;
            const ModbusDataType type = metadata->dataType != TEXT ? metadata->dataType : UINT16;
            const ModbusMqttWriteCore::ConversionResult conversion = ModbusMqttWriteCore::convertPayload(
                function, std::string(request.writeText.c_str()),
                static_cast<double>(metadata->scale), type, metadata->wordOrder);
            if (!conversion.accepted()) {
                result.operation = ModbusOperationResult::Success;
                result.busStatus = ModbusMaster::ku8MBIllegalDataValue;
                result.writeRejection = conversion.rejection;
                return result;
            }
            std::copy(conversion.words.begin(), conversion.words.end(), words.begin());
            wordCount = conversion.wordCount;
        }
        // A 0x44 write given as raw words (HTTP execute `values`) may span
        // several registers, e.g. a whole clock page; an encoded value is one.
        const bool rawWavinWords = request.function == WAVIN_WRITE && request.writeCount > 0U;
        if (wordCount == 0U || wordCount > words.size()
            || (isSingleRegisterWriteCode(request.function) && !rawWavinWords && wordCount != 1U)) {
            result.operation = ModbusOperationResult::Success;
            result.busStatus = ModbusMaster::ku8MBIllegalDataValue;
            result.writeRejection = ModbusMqttWriteCore::RejectionReason::UnsupportedDataType;
            return result;
        }
        // The written words are echoed as full registers of the type they
        // were encoded with, never through a byte slice or as ASCII.
        result.registerSlice = RegisterSlice::Full;
        if (result.dataType == TEXT) result.dataType = UINT16;
    }

    auto guard = _bus.port().acquire();
    if (!guard) {
        result.operation = ModbusOperationResult::Success;
        result.busStatus = 0xE4U;
        return result;
    }
    _bus.port().enableCapture(true);
    ModbusMaster &node = _bus.node();
    node.begin(result.slaveId, _bus.port().stream());
    const uint16_t effectiveLength = request.length;
    // Wavin replies are parsed here rather than by ModbusMaster, and may
    // carry fewer registers than requested.
    std::array<uint16_t, WavinFrame::kMaxRegisters> wavinWords{};
    uint8_t wavinReceived = 0U;
    switch (request.function) {
        case 1: result.busStatus = node.readCoils(request.address, effectiveLength); break;
        case 2: result.busStatus = node.readDiscreteInputs(request.address, effectiveLength); break;
        case 3: result.busStatus = node.readHoldingRegisters(request.address, effectiveLength); break;
        case 4: result.busStatus = node.readInputRegisters(request.address, effectiveLength); break;
        case 5:
            // ModbusMaster::writeSingleCoil(uint16_t, uint8_t) expects the coil
            // state as 0/1 and builds the 0xFF00/0x0000 wire value itself.
            // Passing 0xFF00 truncated to 0 and switched every coil off.
            result.busStatus = node.writeSingleCoil(request.address, coilOn ? 1U : 0U);
            break;
        case 6:
            result.busStatus = node.writeSingleRegister(request.address, words[0]);
            break;
        case 16:
            // One register per 16-bit word of the data type: 1, 2 or 4.
            for (uint8_t i = 0; i < wordCount; ++i) node.setTransmitBuffer(i, words[i]);
            result.busStatus = node.writeMultipleRegisters(request.address, wordCount);
            break;
        case WAVIN_READ:
            result.busStatus = WavinClient(_bus.port()).readRegisters(
                result.slaveId, wavinCategory, static_cast<uint8_t>(request.address), wavinPage,
                static_cast<uint8_t>(effectiveLength), wavinWords.data(), wavinWords.size(),
                wavinReceived);
            break;
        case WAVIN_WRITE:
            result.busStatus = WavinClient(_bus.port()).writeRegisters(
                result.slaveId, wavinCategory, static_cast<uint8_t>(request.address), wavinPage,
                words.data(), wordCount);
            break;
        default:
            result.busStatus = ModbusMaster::ku8MBIllegalFunction;
            break;
    }

    if (result.busStatus == ModbusMaster::ku8MBSuccess && expectedRead) {
        const uint16_t available = wavin ? wavinReceived : effectiveLength;
        result.count = available < result.words.size()
                           ? available
                           : static_cast<uint16_t>(result.words.size());
        for (uint16_t i = 0; i < result.count; ++i) {
            result.words[i] = wavin ? wavinWords[i] : node.getResponseBuffer(i);
        }
    } else if (result.busStatus == ModbusMaster::ku8MBSuccess && registerWrite) {
        // Echo the register words that were written so callers can confirm
        // the encoding.
        result.count = wordCount;
        for (uint16_t i = 0; i < wordCount; ++i) result.words[i] = words[i];
    }
    result.rxDump = _bus.port().dumpRx();
    result.operation = ModbusOperationResult::Success;
    if (result.busStatus != ModbusMaster::ku8MBSuccess) incrementBusErrorCount();
    return result;
}

void ModbusManager::setMqttManager(MqttManager *mqtt) {
    _mqtt = mqtt;
    _mqttBridge.setMqttManager(mqtt);
}

ModbusRuntimeSnapshot ModbusManager::getRuntimeSnapshot() const {
    return _runtimeSnapshot.read();
}

void ModbusManager::updateRuntimeSnapshot() {
    ModbusRuntimeSnapshot snapshot;
    snapshot.available = _ownerAvailable.load(std::memory_order_acquire);
    snapshot.enabled = _bus.isActive();
    snapshot.generation = _generation;
    snapshot.deviceCount = _modbusRoot.devices.size();
    size_t datapointCount = 0U;
    for (const auto &device : _modbusRoot.devices) datapointCount += device.datapoints.size();
    snapshot.datapointCount = datapointCount;
    snapshot.errorCount = _bus.port().errorCount();
    snapshot.ownerViolationCount = _ownerViolationCount.load(std::memory_order_relaxed);
    snapshot.commandFailureCount = _commandFailureCount.load(std::memory_order_relaxed);
    snapshot.commandQueueDepth = (_commandQueue != nullptr ? uxQueueMessagesWaiting(_commandQueue) : 0U)
                               + (_controlQueue != nullptr ? uxQueueMessagesWaiting(_controlQueue) : 0U);
    snapshot.otaQuiesced = _otaQuiesced;
    snapshot.otaOperationId = _otaOperationId;
    snapshot.otaQuiesceResult = _otaQuiesceResult;
    _runtimeSnapshot.write(std::move(snapshot));
}

bool ModbusManager::inOwnerContext() const {
    return _ownerTaskHandle == nullptr || xTaskGetCurrentTaskHandle() == _ownerTaskHandle;
}

void ModbusManager::recordOwnerViolation() {
    _ownerViolationCount.fetch_add(1U, std::memory_order_relaxed);
    _logger->logError("[Modbus] Owner-context violation blocked");
}

void ModbusManager::incrementBusErrorCount() {
    _bus.port().incrementError();
}

const char *ModbusManager::statusToString(const uint8_t code) {
    switch (code) {
        case 0x00: return "Success";
        case 0x01: return "IllegalFunction(0x01)";
        case 0x02: return "IllegalDataAddress(0x02)";
        case 0x03: return "IllegalDataValue(0x03)";
        case 0x04: return "SlaveDeviceFailure(0x04)";
        case 0xE0: return "InvalidSlaveID(0xE0)";
        case 0xE1: return "InvalidFunction(0xE1)";
        case 0xE2: return "ResponseTimedOut(0xE2)";
        case 0xE3: return "InvalidCRC(0xE3)";
        case 0xE4: return "Busy";
        default: return "Unknown";
    }
}

const char *ModbusManager::functionToString(const ModbusFunctionType function) {
    switch (function) {
        case READ_COIL: return "FC01-READ_COIL";
        case READ_DISCRETE: return "FC02-READ_DISCRETE";
        case READ_HOLDING: return "FC03-READ_HOLDING";
        case READ_INPUT: return "FC04-READ_INPUT";
        case WRITE_COIL: return "FC05-WRITE_COIL";
        case WRITE_HOLDING: return "FC06-WRITE_HOLDING";
        case WRITE_MULTIPLE_HOLDING: return "FC16-WRITE_MULTIPLE_HOLDING";
        case WAVIN_READ: return "FC67-WAVIN_READ(0x43)";
        case WAVIN_WRITE: return "FC68-WAVIN_WRITE(0x44)";
    }
    return "FC-UNKNOWN";
}

String ModbusManager::registersToAscii(const uint16_t *buffer, const uint16_t count) {
    String output;
    if (buffer == nullptr || count == 0U) return output;
    output.reserve(count * 2U);
    for (uint16_t i = 0; i < count; ++i) {
        const char high = static_cast<char>((buffer[i] >> 8U) & 0xFFU);
        const char low = static_cast<char>(buffer[i] & 0xFFU);
        if (high != '\0') output += high;
        if (low != '\0') output += low;
    }
    return output;
}
