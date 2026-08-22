#include "modbus/ModbusMqttBridge.h"

#include <ArduinoJson.h>

#include "modbus/ModbusFunctionUtils.h"
#include "modbus/ModbusManager.h"
#include "modbus/ModbusMqttWriteCore.h"
#include "modbus/ModbusTopicBuilder.h"
#include "mqtt/MqttManager.h"

ModbusMqttBridge::ModbusMqttBridge(Logger *logger, ModbusManager *modbus)
    : _logger(logger), _modbus(modbus) {
}

void ModbusMqttBridge::setMqttManager(MqttManager *mqtt) {
    _mqtt = mqtt;
}

MqttBridgePlan ModbusMqttBridge::buildBridgePlan(const ConfigurationRoot &root,
                                                 const uint32_t modbusGeneration) const {
    MqttBridgePlan plan;
    plan.modbusGeneration = modbusGeneration;
    const ModbusTopicBuilder relativeBuilder("");

    bool hasWill = false;
    for (const auto &device : root.devices) {
        if (!device.mqttEnabled) continue;
        if (device.homeassistantDiscoveryEnabled && !hasWill) {
            plan.will.configured = true;
            plan.will.topic = {relativeBuilder.availabilityTopic(device), true};
            plan.will.payload = "offline";
            plan.will.qos = 1U;
            plan.will.retain = true;
            hasWill = true;
        }

        for (const auto &datapoint : device.datapoints) {
            if (isReadOnlyFunction(datapoint.function)) continue;

            String explicitTopic = datapoint.topic;
            explicitTopic.trim();
            MqttTopicSpec topic;
            topic.relativeToRoot = !explicitTopic.length();
            topic.topic = topic.relativeToRoot
                              ? relativeBuilder.datapointTopic(device, datapoint)
                              : explicitTopic;

            const uint8_t slaveId = device.slaveId;
            const auto function = datapoint.function;
            const uint16_t address = datapoint.address;
            const uint8_t registerCount = datapoint.numOfRegisters ? datapoint.numOfRegisters : 1U;
            const float scale = datapoint.scale;
            ModbusMqttWriteCore::CommandContext context;
            context.slaveId = slaveId;
            context.function = function;
            context.address = address;
            context.registerCount = registerCount;
            context.scale = scale;
            context.modbusGeneration = modbusGeneration;
            const auto handler = ModbusMqttWriteCore::buildMessageHandler(
                context,
                [this](const ModbusMqttWriteCore::WriteCommand &command) {
                    if (_modbus == nullptr) return;
                    ModbusCommandRequest request;
                    request.sourceTopic = command.sourceTopic.c_str();
                    request.slaveId = command.slaveId;
                    request.hasSlaveOverride = true;
                    request.function = static_cast<int>(command.function);
                    request.address = command.address;
                    request.length = command.registerCount;
                    request.writeValue = command.rawValue;
                    request.hasWriteValue = true;
                    request.expectedGeneration = command.modbusGeneration;
                    const ModbusOperationResult result = _modbus->submitWriteCommand(request);
                    if (result != ModbusOperationResult::Success && _logger != nullptr) {
                        _logger->logWarning((String("[Modbus][MQTT] Command was not admitted: topic=")
                                             + request.sourceTopic + ", function=" + String(request.function)
                                             + ", address=" + String(request.address) + ", reason="
                                             + modbusOperationResultToString(result)).c_str());
                    }
                },
                [this](const std::string &message) {
                    if (_logger != nullptr) _logger->logWarning(message.c_str());
                });
            plan.subscriptions.push_back({topic,
                [handler](const String &commandTopic, const String &payload) {
                    handler(std::string(commandTopic.c_str(), commandTopic.length()),
                            std::string(payload.c_str(), payload.length()));
                }});
        }
    }
    return plan;
}

MqttOperationResult ModbusMqttBridge::stageConfiguration(const ConfigurationRoot &root,
                                                         const uint32_t modbusGeneration) {
    if (_mqtt == nullptr) return MqttOperationResult::Unavailable;
    return _mqtt->stageBridgePlan(buildBridgePlan(root, modbusGeneration));
}

void ModbusMqttBridge::commitConfiguration(const uint32_t modbusGeneration) {
    if (_mqtt != nullptr) _mqtt->approveBridgePlan(modbusGeneration);
}

void ModbusMqttBridge::onConfigurationActivated(ConfigurationRoot &root) {
    for (auto &device : root.devices) {
        device.haAvailabilityOnlinePublished = false;
        device.haDiscoveryPublished = false;
        device.haAvailabilityPublishPending = false;
        device.haAvailabilityPublishAttempt.invalidate();
        device.haDiscoveryPublishPending = 0U;
        device.haDiscoveryPublishAttempt.invalidate();
        device.haDiscoveryPublishFailed = false;
        device.haDiscoveryPublishCursor = 0U;
        device.haDiscoveryPublishSchedulingComplete = false;
    }
    _lastConnectionEpoch = 0U;
}

void ModbusMqttBridge::onConnectionState(const MqttStatusSnapshot &mqttStatus,
                                         ConfigurationRoot &root,
                                         const uint32_t modbusGeneration) {
    const bool matchingGeneration = mqttStatus.modbusGeneration == modbusGeneration;
    if (mqttStatus.connected && matchingGeneration
        && mqttStatus.connectionEpoch != _lastConnectionEpoch) {
        handleMqttDisconnected(root);
        _lastConnectionEpoch = mqttStatus.connectionEpoch;
    } else if ((!mqttStatus.connected || !matchingGeneration) && _lastConnectionEpoch != 0U) {
        handleMqttDisconnected(root);
        _lastConnectionEpoch = 0U;
    }
    if (mqttStatus.connected && matchingGeneration) {
        handleMqttConnected(root, modbusGeneration);
    }
}

void ModbusMqttBridge::handleMqttConnected(ConfigurationRoot &root, const uint32_t modbusGeneration) {
    if (_mqtt == nullptr) return;
    const MqttStatusSnapshot status = _mqtt->getStatusSnapshot();
    if (!status.enabled || !status.connected || status.modbusGeneration != modbusGeneration) return;

    for (size_t deviceIndex = 0U; deviceIndex < root.devices.size(); ++deviceIndex) {
        auto &device = root.devices[deviceIndex];
        if (!device.mqttEnabled || !device.homeassistantDiscoveryEnabled) continue;
        publishAvailabilityOnline(device, deviceIndex, status, modbusGeneration);
        publishHomeAssistantDiscovery(device, deviceIndex, status, modbusGeneration);
    }
}

void ModbusMqttBridge::handleMqttDisconnected(ConfigurationRoot &root) {
    for (auto &device : root.devices) {
        device.haAvailabilityOnlinePublished = false;
        device.haDiscoveryPublished = false;
        device.haAvailabilityPublishPending = false;
        device.haAvailabilityPublishAttempt.invalidate();
        device.haDiscoveryPublishPending = 0U;
        device.haDiscoveryPublishAttempt.invalidate();
        device.haDiscoveryPublishFailed = false;
        device.haDiscoveryPublishCursor = 0U;
        device.haDiscoveryPublishSchedulingComplete = false;
    }
}

String ModbusMqttBridge::buildDatapointTopic(const ModbusDevice &device,
                                             const ModbusDatapoint &datapoint,
                                             const String &rootTopic) const {
    return ModbusTopicBuilder(rootTopic).datapointTopic(device, datapoint);
}

String ModbusMqttBridge::buildAvailabilityTopic(const ModbusDevice &device,
                                                const String &rootTopic) const {
    return ModbusTopicBuilder(rootTopic).availabilityTopic(device);
}

void ModbusMqttBridge::publishDatapoint(ModbusDevice &device,
                                        const ModbusDatapoint &datapoint,
                                        const String &payload,
                                        const uint32_t modbusGeneration) const {
    if (!device.mqttEnabled || _mqtt == nullptr || datapoint.id.isEmpty()) return;
    const MqttStatusSnapshot status = _mqtt->getStatusSnapshot();
    if (!status.enabled || !status.connected || status.modbusGeneration != modbusGeneration) return;

    String topic = buildDatapointTopic(device, datapoint, status.rootTopic);
    topic.trim();
    if (!topic.length()) return;

    const MqttAdmissionResult admission = _mqtt->publishAsync(
        {topic, payload, false, status.generation, modbusGeneration, status.connectionEpoch});
    if (admission != MqttAdmissionResult::Accepted) {
        _logger->logWarning((String("MQTT publish was not admitted for topic ") + topic + ": "
                             + mqttAdmissionResultToString(admission)).c_str());
    }
}

MqttPublishCallback ModbusMqttBridge::buildPublishCallback(
    const size_t deviceIndex,
    const ModbusPublicationKind kind,
    const uint32_t modbusGeneration,
    const uint32_t connectionEpoch,
    const uint32_t attempt) const {
    return [this, deviceIndex, kind, modbusGeneration, connectionEpoch, attempt]
        (const MqttOperationResult result) {
            if (_modbus == nullptr) return;
            ModbusPublicationCompletion completion;
            completion.deviceIndex = deviceIndex;
            completion.kind = kind;
            completion.result = result;
            completion.modbusGeneration = modbusGeneration;
            completion.connectionEpoch = connectionEpoch;
            completion.attempt = attempt;
            if (_modbus->submitPublicationCompletion(completion) != ModbusOperationResult::Success) {
                _publicationCompletionLost.store(true, std::memory_order_release);
            }
        };
}

void ModbusMqttBridge::publishAvailabilityOnline(ModbusDevice &device,
                                                 const size_t deviceIndex,
                                                 const MqttStatusSnapshot &mqttStatus,
                                                 const uint32_t modbusGeneration) const {
    if (!device.homeassistantDiscoveryEnabled || !device.mqttEnabled || _mqtt == nullptr
        || !mqttStatus.enabled || !mqttStatus.connected
        || device.haAvailabilityOnlinePublished || device.haAvailabilityPublishPending) return;

    String topic = buildAvailabilityTopic(device, mqttStatus.rootTopic);
    topic.trim();
    if (!topic.length()) return;
    const uint32_t attempt = device.haAvailabilityPublishAttempt.begin();
    const MqttAdmissionResult admission = _mqtt->publishAsync(
        {topic, "online", true, mqttStatus.generation, modbusGeneration,
         mqttStatus.connectionEpoch},
        buildPublishCallback(deviceIndex, ModbusPublicationKind::Availability,
                             modbusGeneration, mqttStatus.connectionEpoch, attempt));
    if (admission == MqttAdmissionResult::Accepted) {
        device.haAvailabilityPublishPending = true;
    } else {
        _logger->logWarning((String("[MQTT][HA] Availability publish was not admitted: ")
                             + mqttAdmissionResultToString(admission)).c_str());
    }
}

void ModbusMqttBridge::publishHomeAssistantDiscovery(ModbusDevice &device,
                                                     const size_t deviceIndex,
                                                     const MqttStatusSnapshot &mqttStatus,
                                                     const uint32_t modbusGeneration) const {
    if (!device.homeassistantDiscoveryEnabled || !device.mqttEnabled || _mqtt == nullptr
        || !mqttStatus.enabled || !mqttStatus.connected || device.haDiscoveryPublished
        || device.haDiscoveryPublishPending != 0U) return;

    const String deviceSegment = ModbusTopicBuilder::deviceSegment(device);
    const String availabilityTopic = buildAvailabilityTopic(device, mqttStatus.rootTopic);
    String deviceIdentifier = device.id;
    deviceIdentifier.trim();
    if (!deviceIdentifier.length()) deviceIdentifier = deviceSegment;

    bool anyEligible = false;
    for (const auto &datapoint : device.datapoints) {
        anyEligible = anyEligible || isReadOnlyFunction(datapoint.function)
                      || isWriteFunction(datapoint.function);
    }
    if (!anyEligible) {
        device.haDiscoveryPublished = true;
        return;
    }
    if (device.haDiscoveryPublishCursor == 0U) {
        device.haDiscoveryPublishFailed = false;
        device.haDiscoveryPublishSchedulingComplete = false;
        device.haDiscoveryPublishAttempt.begin();
    }
    const uint32_t attempt = device.haDiscoveryPublishAttempt.current();
    auto findStateTopicForCommand = [&](const String &commandTopic) -> String {
        for (const auto &candidate : device.datapoints) {
            if (!isReadOnlyFunction(candidate.function)) continue;
            String candidateTopic = buildDatapointTopic(device, candidate, mqttStatus.rootTopic);
            candidateTopic.trim();
            if (candidateTopic == commandTopic) return candidateTopic;
        }
        return {};
    };

    for (size_t datapointIndex = device.haDiscoveryPublishCursor;
         datapointIndex < device.datapoints.size(); ++datapointIndex) {
        const auto &datapoint = device.datapoints[datapointIndex];
        const bool readable = isReadOnlyFunction(datapoint.function);
        const bool writeable = isWriteFunction(datapoint.function);
        if (!readable && !writeable) {
            device.haDiscoveryPublishCursor = datapointIndex + 1U;
            continue;
        }

        String datapointTopic = buildDatapointTopic(device, datapoint, mqttStatus.rootTopic);
        datapointTopic.trim();
        if (!datapointTopic.length()) {
            device.haDiscoveryPublishFailed = true;
            device.haDiscoveryPublishCursor = datapointIndex + 1U;
            continue;
        }

        const String datapointSegment = ModbusTopicBuilder::datapointSegment(datapoint);
        const String baseUniqueId = deviceSegment + "_" + datapointSegment;
        const String uniqueId = writeable ? baseUniqueId + "_cmd" : baseUniqueId;
        const String component = readable ? "sensor" : datapoint.function == WRITE_COIL ? "switch" : "number";
        String discoveryTopic;

        JsonDocument document;
        document["name"] = ModbusTopicBuilder::friendlyName(device, datapoint);
        document["unique_id"] = uniqueId;
        document["default_entity_id"] = component + "." + uniqueId;
        document["availability_topic"] = availabilityTopic;
        document["payload_available"] = "online";
        document["payload_not_available"] = "offline";
        auto deviceObject = document["device"].to<JsonObject>();
        deviceObject["identifiers"].to<JsonArray>().add(deviceIdentifier);
        deviceObject["name"] = device.name.length() ? device.name : deviceSegment;

        if (readable) {
            discoveryTopic = String("homeassistant/sensor/") + deviceSegment + "/" + datapointSegment + "/config";
            document["state_topic"] = datapointTopic;
            if (datapoint.unit.length()) document["unit_of_measurement"] = datapoint.unit;
            if (datapoint.function == READ_HOLDING) document["state_class"] = "measurement";
        } else if (datapoint.function == WRITE_COIL) {
            discoveryTopic = String("homeassistant/switch/") + deviceSegment + "/" + datapointSegment + "/config";
            document["command_topic"] = datapointTopic;
            document["payload_on"] = "1";
            document["payload_off"] = "0";
            const String stateTopic = findStateTopicForCommand(datapointTopic);
            if (stateTopic.length()) document["state_topic"] = stateTopic;
            else document["optimistic"] = true;
        } else {
            discoveryTopic = String("homeassistant/number/") + deviceSegment + "/" + datapointSegment + "/config";
            document["command_topic"] = datapointTopic;
            const String stateTopic = findStateTopicForCommand(datapointTopic);
            if (stateTopic.length()) document["state_topic"] = stateTopic;
            else document["optimistic"] = true;
            if (datapoint.unit.length()) document["unit_of_measurement"] = datapoint.unit;
            const auto bounds = ModbusMqttWriteCore::numberDiscoveryBounds(datapoint.scale);
            if (!bounds.valid()) {
                if (_logger != nullptr) {
                    _logger->logWarning((String("[MQTT][HA] Number discovery rejected: function=")
                                         + String(static_cast<int>(datapoint.function))
                                         + ", address=" + String(datapoint.address) + ", reason="
                                         + ModbusMqttWriteCore::rejectionReasonToString(bounds.rejection)).c_str());
                }
                device.haDiscoveryPublishFailed = true;
                device.haDiscoveryPublishCursor = datapointIndex + 1U;
                continue;
            }
            document["min"] = bounds.minimum;
            document["max"] = bounds.maximum;
            document["step"] = bounds.step;
            document["mode"] = "box";
        }

        String payload;
        serializeJson(document, payload);
        const MqttAdmissionResult admission = _mqtt->publishAsync(
            {discoveryTopic, payload, true, mqttStatus.generation, modbusGeneration,
             mqttStatus.connectionEpoch},
            buildPublishCallback(deviceIndex, ModbusPublicationKind::Discovery,
                                 modbusGeneration, mqttStatus.connectionEpoch, attempt));
        if (admission == MqttAdmissionResult::Accepted) {
            ++device.haDiscoveryPublishPending;
            device.haDiscoveryPublishCursor = datapointIndex + 1U;
        } else {
            _logger->logWarning((String("[MQTT][HA] Discovery publish was not admitted: ")
                                 + mqttAdmissionResultToString(admission)).c_str());
            if (admission == MqttAdmissionResult::QueueFull) return;
            device.haDiscoveryPublishFailed = true;
            device.haDiscoveryPublishCursor = datapointIndex + 1U;
        }
    }

    device.haDiscoveryPublishSchedulingComplete = true;
    if (device.haDiscoveryPublishPending == 0U) {
        device.haDiscoveryPublished = !device.haDiscoveryPublishFailed;
        if (!device.haDiscoveryPublished) {
            device.haDiscoveryPublishCursor = 0U;
            device.haDiscoveryPublishSchedulingComplete = false;
        }
    }
}

void ModbusMqttBridge::onPublicationCompletion(
    ConfigurationRoot &root,
    const ModbusPublicationCompletion &completion,
    const uint32_t modbusGeneration) {
    if (completion.modbusGeneration == 0U
        || completion.modbusGeneration != modbusGeneration
        || completion.connectionEpoch == 0U
        || completion.connectionEpoch != _lastConnectionEpoch
        || completion.deviceIndex >= root.devices.size()) return;

    auto &device = root.devices[completion.deviceIndex];
    if (completion.kind == ModbusPublicationKind::Availability) {
        if (!device.haAvailabilityPublishPending
            || !device.haAvailabilityPublishAttempt.accepts(completion.attempt)) return;
        device.haAvailabilityPublishPending = false;
        device.haAvailabilityOnlinePublished = completion.result == MqttOperationResult::Success;
        return;
    }

    if (device.haDiscoveryPublishPending == 0U
        || !device.haDiscoveryPublishAttempt.accepts(completion.attempt)) return;
    --device.haDiscoveryPublishPending;
    if (completion.result != MqttOperationResult::Success) {
        device.haDiscoveryPublishFailed = true;
    }
    if (device.haDiscoveryPublishPending == 0U
        && device.haDiscoveryPublishSchedulingComplete) {
        device.haDiscoveryPublished = !device.haDiscoveryPublishFailed;
        if (!device.haDiscoveryPublished) {
            device.haDiscoveryPublishCursor = 0U;
            device.haDiscoveryPublishSchedulingComplete = false;
        }
    }
}

void ModbusMqttBridge::recoverLostPublicationCompletions(ConfigurationRoot &root) {
    if (!_publicationCompletionLost.exchange(false, std::memory_order_acq_rel)) return;
    _logger->logWarning("[MQTT] Publication completion queue overflow; retrying pending metadata");
    for (auto &device : root.devices) {
        device.haAvailabilityOnlinePublished = false;
        device.haDiscoveryPublished = false;
        device.haAvailabilityPublishPending = false;
        device.haAvailabilityPublishAttempt.invalidate();
        device.haDiscoveryPublishPending = 0U;
        device.haDiscoveryPublishAttempt.invalidate();
        device.haDiscoveryPublishFailed = false;
        device.haDiscoveryPublishCursor = 0U;
        device.haDiscoveryPublishSchedulingComplete = false;
    }
}
