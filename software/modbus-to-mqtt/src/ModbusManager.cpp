#include "modbus/ModbusManager.h"

#include <cmath>
#include <new>
#include <utility>

#include "concurrency/OwnerRequest.h"
#include "modbus/ModbusConfigLoader.h"
#include "modbus/ModbusPollScheduler.h"
#include "mqtt/MqttManager.h"
#include "services/IndicatorService.h"
#include "storage/ConfigFs.h"

struct ModbusManager::Command final : OwnerRequest {
    enum class Type : uint8_t {
        ActivateConfiguration,
        Execute,
        SetEnabled,
        PublicationCompletion,
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
};

ModbusManager::ModbusManager(Logger *logger)
    : _bus(logger),
      _mqttBridge(logger, this),
      _logger(logger) {
}

bool ModbusManager::begin() {
    _ownerTaskHandle = xTaskGetCurrentTaskHandle();
    _commandQueue = xQueueCreate(MODBUS_COMMAND_QUEUE_DEPTH, sizeof(Command *));
    if (_commandQueue == nullptr) {
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

ModbusOperationResult ModbusManager::submitPublicationCompletion(
    const ModbusPublicationCompletion &completion) {
    auto *command = new (std::nothrow) Command(Command::Type::PublicationCompletion);
    if (command == nullptr) return ModbusOperationResult::Unavailable;
    command->publicationCompletion = completion;
    return submitAsync(command);
}

void ModbusManager::drainCommands() {
    for (size_t i = 0; i < MODBUS_COMMANDS_PER_CYCLE; ++i) {
        Command *command = nullptr;
        if (xQueueReceive(_commandQueue, &command, 0) != pdTRUE) break;
        if (command != nullptr && command->tryStart()) {
            processCommand(*command);
            command->complete();
        }
        if (command != nullptr) command->release();
    }
}

void ModbusManager::processCommand(Command &command) {
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
    }
    if (command.result != ModbusOperationResult::Success
        && command.result != ModbusOperationResult::Superseded
        && command.result != ModbusOperationResult::StaleGeneration) {
        _commandFailureCount.fetch_add(1U, std::memory_order_relaxed);
    }
    updateRuntimeSnapshot();
}

void ModbusManager::loop() {
    if (!inOwnerContext()) {
        recordOwnerViolation();
        return;
    }

    // This is the only activation/command boundary. Once polling begins below,
    // no queued request is observed until every device/datapoint in the pass is
    // complete and all local pointers have gone out of scope.
    drainCommands();

    _mqttBridge.recoverLostPublicationCompletions(_modbusRoot);

    if (_mqtt != nullptr) {
        const MqttStatusSnapshot mqttStatus = _mqtt->getStatusSnapshot();
        _mqttBridge.onConnectionState(mqttStatus, _modbusRoot, _generation);
    }

    if (!_bus.isActive()) {
        IndicatorService::instance().setModbusConnected(false);
        updateRuntimeSnapshot();
        return;
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
    }
    if (anyAttempted) IndicatorService::instance().setModbusConnected(anySuccess);
    updateRuntimeSnapshot();
}

bool ModbusManager::readModbusDevice(ModbusDevice &device,
                                     const std::vector<ModbusDatapoint *> &dueDatapoints,
                                     const uint32_t now) {
    auto guard = _bus.acquire();
    if (!guard) return false;

    ModbusMaster &node = _bus.node();
    node.begin(device.slaveId, _bus.stream());
    bool successOnDevice = false;
    for (auto *datapointPointer : dueDatapoints) {
        if (datapointPointer == nullptr) continue;
        auto &datapoint = *datapointPointer;
        uint8_t result = ModbusMaster::ku8MBIllegalFunction;
        switch (datapoint.function) {
            case READ_COIL: result = node.readCoils(datapoint.address, datapoint.numOfRegisters); break;
            case READ_DISCRETE: result = node.readDiscreteInputs(datapoint.address, datapoint.numOfRegisters); break;
            case READ_HOLDING: result = node.readHoldingRegisters(datapoint.address, datapoint.numOfRegisters); break;
            case READ_INPUT: result = node.readInputRegisters(datapoint.address, datapoint.numOfRegisters); break;
            case WRITE_COIL:
            case WRITE_HOLDING:
            case WRITE_MULTIPLE_HOLDING:
                ModbusPollScheduler::scheduleNext(datapoint, now);
                continue;
        }

        if (result == ModbusMaster::ku8MBSuccess) {
            successOnDevice = true;
            const uint8_t wordCount = datapoint.numOfRegisters ? datapoint.numOfRegisters : 1U;
            std::vector<uint16_t> words(wordCount);
            for (uint8_t i = 0; i < wordCount; ++i) words[i] = node.getResponseBuffer(i);
            String payload;
            if (datapoint.dataType == TEXT) {
                payload = registersToAscii(words.data(), wordCount);
            } else {
                const uint16_t primary = wordCount > 0U ? words[0] : 0U;
                payload = String(static_cast<float>(sliceRegister(primary, datapoint.registerSlice))
                                 * datapoint.scale);
            }
            _mqttBridge.publishDatapoint(device, datapoint, payload, _generation);
        } else {
            incrementBusErrorCount();
            _logger->logError((String("Modbus ERR - ") + device.name + ": func="
                               + functionToString(datapoint.function) + ", addr=" + String(datapoint.address)
                               + ", code=" + String(result) + " (" + statusToString(result) + ")"
                               + _bus.dumpRx()).c_str());
        }
        ModbusPollScheduler::scheduleNext(datapoint, now);
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

    const ModbusDatapoint *metadata = nullptr;
    const ModbusDevice *metadataDevice = nullptr;
    for (const auto &device : _modbusRoot.devices) {
        if (metadataDevice == nullptr && device.id == request.deviceId) metadataDevice = &device;
        for (const auto &datapoint : device.datapoints) {
            if (datapoint.id == request.datapointId) {
                metadata = &datapoint;
                metadataDevice = &device;
                break;
            }
        }
        if (metadata != nullptr) break;
    }
    if (metadata != nullptr) {
        result.hasDatapointMetadata = true;
        result.dataType = metadata->dataType;
        result.registerSlice = metadata->registerSlice;
        result.scale = metadata->scale;
    }

    result.slaveId = request.hasSlaveOverride ? request.slaveId
                     : metadataDevice != nullptr ? metadataDevice->slaveId
                                                 : MODBUS_SLAVE_ID;
    const bool expectedWrite = request.function == 5 || request.function == 6 || request.function == 16;
    const bool expectedRead = request.function >= 1 && request.function <= 4;
    if ((expectedWrite && !request.hasWriteValue) || (!expectedRead && !expectedWrite)) {
        result.operation = ModbusOperationResult::Success;
        result.busStatus = expectedWrite
                               ? ModbusMaster::ku8MBIllegalDataValue
                               : ModbusMaster::ku8MBIllegalFunction;
        return result;
    }

    auto guard = _bus.acquire();
    if (!guard) {
        result.operation = ModbusOperationResult::Success;
        result.busStatus = 0xE4U;
        return result;
    }
    _bus.enableCapture(true);
    ModbusMaster &node = _bus.node();
    node.begin(result.slaveId, _bus.stream());
    const uint16_t effectiveLength = request.function == 16 ? 1U : request.length;
    switch (request.function) {
        case 1: result.busStatus = node.readCoils(request.address, effectiveLength); break;
        case 2: result.busStatus = node.readDiscreteInputs(request.address, effectiveLength); break;
        case 3: result.busStatus = node.readHoldingRegisters(request.address, effectiveLength); break;
        case 4: result.busStatus = node.readInputRegisters(request.address, effectiveLength); break;
        case 5: {
            const uint16_t value = request.writeValue ? 0xFF00U : 0x0000U;
            node.beginTransmission(request.address);
            node.send(value);
            result.busStatus = node.writeSingleCoil(request.address, value);
            break;
        }
        case 6:
            node.beginTransmission(request.address);
            node.send(request.writeValue);
            result.busStatus = node.writeSingleRegister(request.address, request.writeValue);
            break;
        case 16:
            node.setTransmitBuffer(0, request.writeValue);
            result.busStatus = node.writeMultipleRegisters(request.address, effectiveLength);
            break;
        default:
            result.busStatus = ModbusMaster::ku8MBIllegalFunction;
            break;
    }

    if (result.busStatus == ModbusMaster::ku8MBSuccess && expectedRead) {
        result.count = effectiveLength < result.words.size()
                           ? effectiveLength
                           : static_cast<uint16_t>(result.words.size());
        for (uint16_t i = 0; i < result.count; ++i) result.words[i] = node.getResponseBuffer(i);
    }
    result.rxDump = _bus.dumpRx();
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
    snapshot.errorCount = _bus.errorCount();
    snapshot.ownerViolationCount = _ownerViolationCount.load(std::memory_order_relaxed);
    snapshot.commandFailureCount = _commandFailureCount.load(std::memory_order_relaxed);
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
    _bus.incrementError();
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
    }
    return "FC-UNKNOWN";
}

uint16_t ModbusManager::sliceRegister(const uint16_t word, const RegisterSlice slice) {
    switch (slice) {
        case RegisterSlice::LowByte: return static_cast<uint16_t>(word & 0x00FFU);
        case RegisterSlice::HighByte: return static_cast<uint16_t>((word >> 8U) & 0x00FFU);
        case RegisterSlice::Full: return word;
    }
    return word;
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
