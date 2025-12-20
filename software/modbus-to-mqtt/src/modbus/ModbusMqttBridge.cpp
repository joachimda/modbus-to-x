#include "modbus/ModbusMqttBridge.h"

#include "mqtt/MqttManager.h"
#include "modbus/ModbusFunctionUtils.h"
#include "modbus/ModbusManager.h"
#include "modbus/ModbusTopicBuilder.h"
#include <ArduinoJson.h>

ModbusMqttBridge::ModbusMqttBridge(Logger *logger, ModbusManager *modbus)
    : _logger(logger), _modbus(modbus) {
}

void ModbusMqttBridge::setMqttManager(MqttManager *mqtt) {
    _mqtt = mqtt;
}

void ModbusMqttBridge::onConfigurationLoaded(ConfigurationRoot &root) {
    for (auto &device : root.devices) {
        device.haAvailabilityOnlinePublished = false;
        device.haDiscoveryPublished = false;
    }

    if (_mqtt) {
        String willTopic;
        bool multipleDiscovery = false;
        for (auto &device: root.devices) {
            if (device.mqttEnabled && device.homeassistantDiscoveryEnabled) {
                if (willTopic.isEmpty()) {
                    willTopic = buildAvailabilityTopic(device);
                } else {
                    multipleDiscovery = true;
                }
            }
        }
        if (willTopic.length()) {
            _mqtt->configureWill(willTopic, "offline", 1, true);
            _logger->logDebug((String("[MQTT][HA] Set LWT topic to ") + willTopic).c_str());
        } else {
            _mqtt->clearWill();
        }
        if (multipleDiscovery) {
            _logger->logWarning(
                "[MQTT][HA] Multiple devices requested Home Assistant discovery; LWT uses the first matched device");
        }
    }

    rebuildWriteSubscriptions(root);
}

void ModbusMqttBridge::onConnectionState(const bool connectedNow,
                                        const bool connectedLast,
                                        ConfigurationRoot &root) {
    if (connectedNow && !connectedLast) {
        handleMqttConnected(root);
    } else if (!connectedNow && connectedLast) {
        handleMqttDisconnected(root);
    }
}

void ModbusMqttBridge::handleMqttConnected(ConfigurationRoot &root) {
    if (!MqttManager::isMQTTEnabled()) {
        return;
    }

    rebuildWriteSubscriptions(root);

    for (auto &device: root.devices) {
        if (!device.mqttEnabled) {
            continue;
        }
        if (device.homeassistantDiscoveryEnabled) {
            publishAvailabilityOnline(device);
            publishHomeAssistantDiscovery(device);
        }
    }
}

void ModbusMqttBridge::handleMqttDisconnected(ConfigurationRoot &root) {
    for (auto &device: root.devices) {
        device.haAvailabilityOnlinePublished = false;
        device.haDiscoveryPublished = false;
    }
}

String ModbusMqttBridge::buildDatapointTopic(const ModbusDevice &device, const ModbusDatapoint &dp) const {
    const String rootTopic = _mqtt->getRootTopic();
    const ModbusTopicBuilder builder(rootTopic);
    return builder.datapointTopic(device, dp);
}

String ModbusMqttBridge::buildAvailabilityTopic(const ModbusDevice &device) const {
    const String rootTopic = _mqtt->getRootTopic();
    const ModbusTopicBuilder builder(rootTopic);
    return builder.availabilityTopic(device);
}

void ModbusMqttBridge::publishDatapoint(ModbusDevice &device,
                                       const ModbusDatapoint &dp,
                                       const String &payload) const {
    if (!device.mqttEnabled || !_mqtt) {
        return;
    }
    if (!MqttManager::isMQTTEnabled()) {
        return;
    }
    if (dp.id.isEmpty()) {
        return;
    }

    if (device.homeassistantDiscoveryEnabled) {
        if (!device.haAvailabilityOnlinePublished) {
            publishAvailabilityOnline(device);
        }
        if (!device.haDiscoveryPublished) {
            publishHomeAssistantDiscovery(device);
        }
        if (!device.haAvailabilityOnlinePublished || !device.haDiscoveryPublished) {
            return;
        }
    }

    String topic = buildDatapointTopic(device, dp);
    topic.trim();
    if (!topic.length()) {
        _logger->logWarning("ModbusMqttBridge::publishDatapoint - empty topic, skipping publish");
        return;
    }

    if (!_mqtt->mqttPublish(topic.c_str(), payload.c_str())) {
        _logger->logWarning((String("MQTT publish failed for topic ") + topic).c_str());
    } else {
        _logger->logDebug((String("MQTT publish ") + topic + " <= " + payload).c_str());
    }
}

void ModbusMqttBridge::rebuildWriteSubscriptions(const ConfigurationRoot &root) {
    if (!_mqtt || !MqttManager::isMQTTEnabled()) return;

    if (!_writeTopics.empty()) {
        _mqtt->removeSubscriptionHandlers(_writeTopics);
        _writeTopics.clear();
    }

    for (const auto &device: root.devices) {
        if (!device.mqttEnabled) continue;

        for (const auto &dp: device.datapoints) {
            if (isReadOnlyFunction(dp.function)) continue;

            String topic = buildDatapointTopic(device, dp);
            topic.trim();
            if (!topic.length()) {
                _logger->logWarning("ModbusMqttBridge::rebuildWriteSubscriptions - empty topic for write datapoint, skipping");
                continue;
            }

            const uint8_t slaveId = device.slaveId;
            const auto fn = dp.function;
            const uint16_t addr = dp.address;
            const uint8_t numRegs = dp.numOfRegisters ? dp.numOfRegisters : 1;
            const float scale = dp.scale;

            _mqtt->addSubscriptionHandler(topic, [this, topic, slaveId, fn, addr, numRegs, scale](const String &payload) {
                handleWriteCommand(topic, slaveId, fn, addr, numRegs, scale, payload);
            });
            _writeTopics.push_back(topic);
        }
    }
}

void ModbusMqttBridge::handleWriteCommand(const String &topic,
                                         const uint8_t slaveId,
                                         const ModbusFunctionType fn,
                                         const uint16_t addr,
                                         const uint8_t numRegs,
                                         const float scale,
                                         const String &payload) const {
    if (!_modbus) {
        _logger->logError("ModbusMqttBridge::handleWriteCommand - no ModbusManager assigned");
        return;
    }

    String trimmed = payload;
    trimmed.trim();

    uint16_t writeValue = 0;
    bool hasWriteValue = false;

    if (fn == WRITE_COIL) {
        if (trimmed.equalsIgnoreCase("true") || trimmed == "1") {
            writeValue = 1;
            hasWriteValue = true;
        } else if (trimmed.equalsIgnoreCase("false") || trimmed == "0") {
            writeValue = 0;
            hasWriteValue = true;
        } else if (trimmed.length()) {
            writeValue = static_cast<uint16_t>(trimmed.toInt());
            writeValue = writeValue ? 1 : 0;
            hasWriteValue = true;
        }
    } else if (fn == WRITE_HOLDING || fn == WRITE_MULTIPLE_HOLDING) {
        if (!trimmed.length()) {
            _logger->logWarning("ModbusMqttBridge::handleWriteCommand - empty payload for holding register write");
            return;
        }
        const float denom = (scale == 0.0f) ? 1.0f : scale;
        const float requested = trimmed.toFloat();
        const float raw = requested / denom;
        float rounded = (raw >= 0.0f) ? (raw + 0.5f) : (raw - 0.5f);
        if (rounded < 0.0f) rounded = 0.0f;
        if (rounded > 65535.0f) rounded = 65535.0f;
        writeValue = static_cast<uint16_t>(rounded);
        hasWriteValue = true;
    } else {
        _logger->logWarning("ModbusMqttBridge::handleWriteCommand - unsupported function");
        return;
    }

    if (!hasWriteValue) {
        _logger->logWarning(
            (String("ModbusMqttBridge::handleWriteCommand - Unable to parse payload for topic [") + topic + "]").c_str());
        return;
    }

    uint16_t outBuf[1]{};
    uint16_t outCount = 0;
    String rxDump;
    const uint8_t effectiveLen = (fn == WRITE_MULTIPLE_HOLDING) ? 1 : numRegs;
    const uint8_t status = _modbus->executeCommand(slaveId,
                                                   static_cast<int>(fn),
                                                   addr,
                                                   effectiveLen,
                                                   writeValue,
                                                   true,
                                                   outBuf,
                                                   0,
                                                   outCount,
                                                   rxDump);

    if (status == ModbusMaster::ku8MBSuccess) {
        _logger->logDebug(
            (String("Modbus write OK - topic=") + topic + ", addr=" + String(addr) + ", value=" +
             String(writeValue)).c_str());
    } else {
        _logger->logError(
            (String("Modbus write ERR - topic=") + topic + ", addr=" + String(addr) +
             ", code=" + String(status) + " (" + ModbusManager::statusToString(status) + ")" +
             (rxDump.length() ? String(", rx=") + rxDump : String(""))).c_str());
    }
}

void ModbusMqttBridge::publishAvailabilityOnline(ModbusDevice &device) const {
    if (!device.homeassistantDiscoveryEnabled || !device.mqttEnabled) {
        return;
    }
    if (!MqttManager::isMQTTEnabled() || !_mqtt || !_mqtt->isConnected()) {
        return;
    }

    String topic = buildAvailabilityTopic(device);
    topic.trim();
    if (!topic.length()) {
        _logger->logWarning("[MQTT][HA] Availability topic empty, skipping publish");
        return;
    }

    if (_mqtt->mqttPublish(topic.c_str(), "online", true)) {
        device.haAvailabilityOnlinePublished = true;
        _logger->logDebug((String("[MQTT][HA] Availability -> ") + topic + " <= online").c_str());
    } else {
        _logger->logWarning((String("[MQTT][HA] Failed to publish availability topic ") + topic).c_str());
    }
}

void ModbusMqttBridge::publishHomeAssistantDiscovery(ModbusDevice &device) const {
    _logger->logDebug((String("[MQTT][HA] Publishing discovery for device ") + device.id).c_str());
    if (!device.homeassistantDiscoveryEnabled || !device.mqttEnabled) {
        return;
    }

    if (!MqttManager::isMQTTEnabled() || !_mqtt || !_mqtt->isConnected()) {
        return;
    }

    const String deviceSegment = ModbusTopicBuilder::deviceSegment(device);
    const String availabilityTopic = buildAvailabilityTopic(device);
    String deviceIdentifier = device.id;
    deviceIdentifier.trim();
    if (!deviceIdentifier.length()) {
        deviceIdentifier = deviceSegment;
    }

    bool anyEligible = false;
    bool anyPublished = false;

    auto findStateTopicForCommand = [&](const String &commandTopic) -> String {
        for (const auto &candidate: device.datapoints) {
            if (!isReadOnlyFunction(candidate.function)) {
                continue;
            }
            String candidateTopic = buildDatapointTopic(device, candidate);
            candidateTopic.trim();
            if (candidateTopic == commandTopic) {
                return candidateTopic;
            }
        }
        return {};
    };

    for (const auto &dp: device.datapoints) {
        const bool readable = isReadOnlyFunction(dp.function);
        const bool writeable = isWriteFunction(dp.function);
        if (!readable && !writeable) {
            continue;
        }
        anyEligible = true;

        String datapointTopic = buildDatapointTopic(device, dp);
        datapointTopic.trim();
        if (!datapointTopic.length()) {
            continue;
        }

        const String datapointSegment = ModbusTopicBuilder::datapointSegment(dp);
        const String baseUniqueId = deviceSegment + "_" + datapointSegment;
        String discoveryTopic;

        JsonDocument doc;
        const String friendlyName = ModbusTopicBuilder::friendlyName(device, dp);
        const String uniqueId = writeable ? baseUniqueId + "_cmd" : baseUniqueId;

        doc["name"] = friendlyName;
        doc["unique_id"] = uniqueId;
        doc["object_id"] = uniqueId;
        doc["availability_topic"] = availabilityTopic;
        doc["payload_available"] = "online";
        doc["payload_not_available"] = "offline";

        auto deviceObj = doc["device"].to<JsonObject>();
        auto identifiers = deviceObj["identifiers"].to<JsonArray>();
        identifiers.add(deviceIdentifier);
        deviceObj["name"] = device.name.length() ? device.name : deviceSegment;

        if (readable) {
            discoveryTopic =
                String("homeassistant/sensor/") + deviceSegment + "/" + datapointSegment + "/config";
            doc["state_topic"] = datapointTopic;
            if (dp.unit.length()) {
                doc["unit_of_measurement"] = dp.unit;
            }
            if (dp.function == READ_HOLDING) {
                doc["state_class"] = "measurement";
            }
        } else if (dp.function == WRITE_COIL) {
            discoveryTopic =
                String("homeassistant/switch/") + deviceSegment + "/" + datapointSegment + "/config";
            doc["command_topic"] = datapointTopic;
            doc["payload_on"] = "1";
            doc["payload_off"] = "0";
            const String stateTopic = findStateTopicForCommand(datapointTopic);
            if (stateTopic.length()) {
                doc["state_topic"] = stateTopic;
            } else {
                doc["optimistic"] = true;
            }
        } else if (dp.function == WRITE_HOLDING || dp.function == WRITE_MULTIPLE_HOLDING) {
            discoveryTopic =
                String("homeassistant/number/") + deviceSegment + "/" + datapointSegment + "/config";
            doc["command_topic"] = datapointTopic;
            const String stateTopic = findStateTopicForCommand(datapointTopic);
            if (stateTopic.length()) {
                doc["state_topic"] = stateTopic;
            } else {
                doc["optimistic"] = true;
            }
            if (dp.unit.length()) {
                doc["unit_of_measurement"] = dp.unit;
            }
            const float effectiveScale = (dp.scale == 0.0f) ? 1.0f : dp.scale;
            const float step = (effectiveScale > 0.0f) ? effectiveScale : 1.0f;
            const float maxValue = 65535.0f * ((effectiveScale > 0.0f) ? effectiveScale : 1.0f);
            doc["min"] = 0;
            doc["max"] = maxValue;
            doc["step"] = step;
            doc["mode"] = "box";
        } else {
            continue;
        }

        String payload;
        serializeJson(doc, payload);
        if (_mqtt->mqttPublish(discoveryTopic.c_str(), payload.c_str(), true)) {
            anyPublished = true;
            _logger->logDebug((String("[MQTT][HA] Discovery -> ") + discoveryTopic).c_str());
        } else {
            _logger->logWarning((String("[MQTT][HA] Failed to publish discovery topic ") + discoveryTopic).c_str());
        }
    }

    if (anyPublished || !anyEligible) {
        device.haDiscoveryPublished = true;
    }
}

