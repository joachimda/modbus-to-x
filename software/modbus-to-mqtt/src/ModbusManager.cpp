#include "Config.h"
#include "modbus/ModbusManager.h"

#include <atomic>
#include <cmath>
#include <map>
#include <vector>
#include <SPIFFS.h>

#include "ArduinoJson.h"
#include "mqtt/MqttManager.h"
#include "services/IndicatorService.h"
#include "modbus/ModbusConfigLoader.h"
#include "utils/StringUtils.h"
#include "utils/TeeStream.h"

static std::atomic<bool> BUS_ACTIVE{false};
static std::atomic<uint32_t> BUS_ERROR_COUNT{0};
static std::atomic<bool> g_modbusBusy{false};

static const std::map<String, uint32_t> communicationModes = {
    {"8N1", SERIAL_8N1},
    {"8N2", SERIAL_8N2},
    {"8E1", SERIAL_8E1},
    {"8E2", SERIAL_8E2},
    {"8O1", SERIAL_8O1},
    {"8O2", SERIAL_8O2}
};

ModbusManager::ModbusManager(Logger *logger) : _logger(logger) {
}
static TeeStream *g_teeSerial1 = nullptr;

bool ModbusManager::begin() {
    if (loadConfiguration()) {
        BUS_ACTIVE.store(true, std::memory_order_release);
        initializeWiring();
        _logger->logInformation("ModbusManager::begin - RS485 bus is ACTIVE");
        return true;
    }
    BUS_ACTIVE.store(false, std::memory_order_release);
    _logger->logInformation("ModbusManager::begin - RS485 bus is INACTIVE");
    return false;
}

bool ModbusManager::loadConfiguration() {
    const bool ok = ModbusConfigLoader::loadConfiguration(_logger, "/conf/config.json", _modbusRoot);
    if (!ok) {
        return false;
    }

    if (_mqtt) {
        String willTopic;
        bool multipleDiscovery = false;
        for (auto &device: _modbusRoot.devices) {
            device.haAvailabilityOnlinePublished = false;
            device.haDiscoveryPublished = false;
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

    if (_mqtt) {
        rebuildWriteSubscriptions();
    }

    _logger->logInformation((String("Loaded config: ") + String(_modbusRoot.devices.size()) + " devices; baud " +
                             String(_modbusRoot.bus.baud) + ", format " + _modbusRoot.bus.serialFormat).c_str());
    return true;
}

void ModbusManager::initializeWiring() const {
    _logger->logDebug("ModbusManager::initialize - Entry");

    pinMode(RS485_DERE_PIN, OUTPUT);

    digitalWrite(RS485_DERE_PIN, LOW);

    Serial1.begin(_modbusRoot.bus.baud,
                  communicationModes.at(_modbusRoot.bus.serialFormat),
                  RX2, TX2);

    if (!g_teeSerial1) {
        g_teeSerial1 = new TeeStream(Serial1, _logger);
    }

    _logger->logDebug("ModbusManager::initialize - Exit");
}

void ModbusManager::loop() {
    const bool mqttConnectedNow = (_mqtt != nullptr) && _mqtt->isConnected();
    if (mqttConnectedNow && !_mqttConnectedLastLoop) {
        handleMqttConnected();
    } else if (!mqttConnectedNow && _mqttConnectedLastLoop) {
        handleMqttDisconnected();
    }
    _mqttConnectedLastLoop = mqttConnectedNow;

    if (!BUS_ACTIVE.load(std::memory_order_acquire)) {
        IndicatorService::instance().setModbusConnected(false);
        return;
    }

    bool anySuccess = false;
    bool anyAttempted = false;

    const uint32_t now = millis();
    for (auto &dev: _modbusRoot.devices) {
        // Check if any datapoint on this device is due
        bool due = false;
        for (const auto &dp: dev.datapoints) {
            if (dp.pollIntervalMs == 0 || now >= dp.nextDueAtMs) {
                due = true;
                break;
            }
        }
        if (!due) continue;
        anyAttempted = true;
        anySuccess = readModbusDevice(dev) || anySuccess;
    }
    if (anyAttempted) {
        IndicatorService::instance().setModbusConnected(anySuccess);
    }
    if (!anySuccess) {
    }
}

bool ModbusManager::readModbusDevice(const ModbusDevice &dev) {
    bool was = false;
    if (!g_modbusBusy.compare_exchange_strong(was, true, std::memory_order_acq_rel)) {
        return false;
    }
    _logger->logDebug(
        ("ModbusManager::readModbusDevice - Reading Device: " + String(dev.name) + " SlaveId: " + String(dev.slaveId)).
        c_str());
    // Use tee stream to capture incoming bytes for diagnostics
    if (g_teeSerial1) {
        node.begin(dev.slaveId, *g_teeSerial1);
    } else {
        node.begin(dev.slaveId, Serial1);
    }
    node.preTransmission(preTransmissionHandler);
    node.postTransmission(postTransmissionHandler);

    uint8_t result;
    bool successOnThisDevice = false;
    const uint32_t now = millis();
    for (auto &dp: const_cast<ModbusDevice &>(dev).datapoints) {
        if (dp.pollIntervalMs > 0 && now < dp.nextDueAtMs) {
            continue;
        }
        _logger->logDebug((String("ModbusManager::readModbusDevice - Sending Command - Func: ") +
                           String(functionToString(dp.function)) + ", Name: " + String(dp.name) +
                           ", Addr: " + String(dp.address) + ", Regs: " + String(dp.numOfRegisters) +
                           ", Slave: " + String(dev.slaveId) + ", Bus: " + String(_modbusRoot.bus.baud) +
                           "," + _modbusRoot.bus.serialFormat).c_str());
        switch (dp.function) {
            case READ_COIL:
                result = node.readCoils(dp.address, dp.numOfRegisters);
                break;
            case READ_DISCRETE:
                result = node.readDiscreteInputs(dp.address, dp.numOfRegisters);
                break;
            case READ_HOLDING:
                result = node.readHoldingRegisters(dp.address, dp.numOfRegisters);
                break;
            case READ_INPUT:
                result = node.readInputRegisters(dp.address, dp.numOfRegisters);
                break;
            default:
                result = -1;
                _logger->logError(
                    ("ModbusManager::readRegisters - Function: " + String(dp.function) + " is not valid in this scope.")
                    .c_str());
        }
        if (result == ModbusMaster::ku8MBSuccess) {
            successOnThisDevice = true;

            const uint8_t wordsToRead = dp.numOfRegisters ? dp.numOfRegisters : 1;
            std::vector<uint16_t> words(wordsToRead);
            for (uint8_t i = 0; i < wordsToRead; ++i) {
                words[i] = node.getResponseBuffer(i);
            }

            String payload;
            if (dp.dataType == TEXT) {
                payload = registersToAscii(words.data(), wordsToRead);
            } else {
                const uint16_t primary = wordsToRead > 0 ? words[0] : 0;
                const uint16_t sliced = sliceRegister(primary, dp.registerSlice);
                const float value = static_cast<float>(sliced) * dp.scale;
                payload = String(value);
            }

            String rawSummary;
            if (dp.dataType == TEXT) {
                rawSummary.reserve(wordsToRead * 7);
                for (uint8_t i = 0; i < wordsToRead; ++i) {
                    if (i > 0) rawSummary += ' ';
                    char buf[7];
                    snprintf(buf, sizeof(buf), "0x%04X", words[i]);
                    rawSummary += buf;
                }
            } else {
                const uint16_t primary = wordsToRead > 0 ? words[0] : 0;
                rawSummary = String(primary);
            }

            _logger->logDebug(("Modbus OK - " + String(dev.name) + ": " + String(dp.name) +
                               " = " + payload + " (raw=" + rawSummary + ")").c_str());
            publishDatapoint(dev, dp, payload);
        } else {
            // Dump captured RX bytes for diagnostics
            String rxDump = String("");
            _logger->logError((String("Modbus ERR - ") + String(dev.name) +
                               ": func=" + functionToString(dp.function) +
                               ", addr=" + String(dp.address) +
                               ", regs=" + String(dp.numOfRegisters) +
                               ", slave=" + String(dev.slaveId) +
                               ", bus=" + String(_modbusRoot.bus.baud) + "," + _modbusRoot.bus.serialFormat +
                               ", code=" + String(result) + " (" + statusToString(result) + ")" + rxDump).c_str());
            incrementBusErrorCount();
        }
        // Schedule next read regardless of success to avoid hammering bus
        if (dp.pollIntervalMs > 0) {
            dp.nextDueAtMs = now + dp.pollIntervalMs;
        } else {
            dp.nextDueAtMs = 0; // always due
        }
    }
    g_modbusBusy.store(false, std::memory_order_release);
    return successOnThisDevice;
}

void ModbusManager::preTransmissionHandler() {
    digitalWrite(RS485_DERE_PIN, HIGH);
    if (g_teeSerial1) g_teeSerial1->enableCapture(false);
    // Allow transceiver to settle before sending
    delayMicroseconds(RS485_DIR_GUARD_US);
}

void ModbusManager::postTransmissionHandler() {
    Serial1.flush();
    digitalWrite(RS485_DERE_PIN, LOW);
    if (g_teeSerial1) g_teeSerial1->enableCapture(true);
    // Small guard so RX is ready before the slave replies
    delayMicroseconds(RS485_DIR_GUARD_US);
}

bool ModbusManager::reconfigureFromFile() {
    _logger->logInformation("ModbusManager::reconfigureFromFile - begin");
    // Stop regular loop polling
    BUS_ACTIVE.store(false, std::memory_order_release);
    // Wait briefly if a read is in progress
    for (int i = 0; i < 50; ++i) {
        if (!g_modbusBusy.load(std::memory_order_acquire)) break;
        delay(5);
    }

    const bool ok = loadConfiguration();
    if (ok) {
        initializeWiring();
        if (_mqtt) {
            rebuildWriteSubscriptions();
        }
        BUS_ACTIVE.store(true, std::memory_order_release);
        _logger->logInformation("ModbusManager::reconfigureFromFile - applied and active");
    } else {
        BUS_ACTIVE.store(false, std::memory_order_release);
        _logger->logError("ModbusManager::reconfigureFromFile - failed to load config; bus inactive");
    }
    return ok;
}

auto ModbusManager::statusToString(const uint8_t code) -> const char * {
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

auto ModbusManager::functionToString(const ModbusFunctionType fn) -> const char * {
    switch (fn) {
        case READ_COIL: return "FC01-READ_COIL";
        case READ_DISCRETE: return "FC02-READ_DISCRETE";
        case READ_HOLDING: return "FC03-READ_HOLDING";
        case READ_INPUT: return "FC04-READ_INPUT";
        case WRITE_COIL: return "FC05-WRITE_COIL";
        case WRITE_HOLDING: return "FC06-WRITE_HOLDING";
        default: return "FC-UNKNOWN";
    }
}

auto ModbusManager::sliceRegister(const uint16_t word, const RegisterSlice slice) -> uint16_t {
    switch (slice) {
        case RegisterSlice::LowByte:
            return static_cast<uint16_t>(word & 0x00FFU);
        case RegisterSlice::HighByte:
            return static_cast<uint16_t>((word >> 8U) & 0x00FFU);
        case RegisterSlice::Full:
        default:
            return word;
    }
}

const ConfigurationRoot &ModbusManager::getConfiguration() const {
    return _modbusRoot;
}

uint8_t ModbusManager::executeCommand(const uint8_t slaveId,
                                      const int function,
                                      const uint16_t addr,
                                      const uint16_t len,
                                      const uint16_t writeValue,
                                      const bool hasWriteValue,
                                      uint16_t *outBuf,
                                      const uint16_t outBufCap,
                                      uint16_t &outCount,
                                      String &rxDump) {
    outCount = 0;
    rxDump = "";
    const bool expectedWrite = (function == 5 || function == 6);
    if (expectedWrite && !hasWriteValue) {
        return ModbusMaster::ku8MBIllegalDataValue;
    }
    const bool expectedRead = (function >= 1 && function <= 4);
    if (!expectedRead && !expectedWrite) {
        return ModbusMaster::ku8MBIllegalFunction;
    }

    // Best-effort ensure wiring is initialized
    if (!g_teeSerial1) {
        if (_modbusRoot.bus.baud == 0) {
            _modbusRoot.bus.baud = DEFAULT_MODBUS_BAUD_RATE;
            _modbusRoot.bus.serialFormat = DEFAULT_MODBUS_MODE;
        }
        initializeWiring();
    }

    bool was = false;
    if (!g_modbusBusy.compare_exchange_strong(was, true, std::memory_order_acq_rel)) {
        return 0xE4; // busy
    }

    if (g_teeSerial1) g_teeSerial1->enableCapture(true);

    // Configure node
    if (g_teeSerial1) {
        node.begin(slaveId, *g_teeSerial1);
    } else {
        node.begin(slaveId, Serial1);
    }
    node.preTransmission(preTransmissionHandler);
    node.postTransmission(postTransmissionHandler);

    uint8_t status;
    switch (function) {
        case 1: status = node.readCoils(addr, len);
            break;
        case 2: status = node.readDiscreteInputs(addr, len);
            break;
        case 3: status = node.readHoldingRegisters(addr, len);
            break;
        case 4: status = node.readInputRegisters(addr, len);
            break;
        case 5: {
            const uint16_t v = writeValue ? 0xFF00 : 0x0000;
            node.beginTransmission(addr);
            node.send(v);
            status = node.writeSingleCoil(addr, v);
            break;
        }
        case 6: {
            node.beginTransmission(addr);
            node.send(writeValue);
            status = node.writeSingleRegister(addr, writeValue);
            break;
        }
        default:
            status = ModbusMaster::ku8MBIllegalFunction;
            break;
    }

    if (status == ModbusMaster::ku8MBSuccess && expectedRead && outBuf && outBufCap > 0) {
        const uint16_t n = (len < outBufCap) ? len : outBufCap;
        for (uint16_t i = 0; i < n; ++i) {
            outBuf[i] = node.getResponseBuffer(i);
        }
        outCount = n;
    }

    if (g_teeSerial1) {
        rxDump = g_teeSerial1->dumpHex();
    }

    if (status != ModbusMaster::ku8MBSuccess) {
        incrementBusErrorCount();
    }

    g_modbusBusy.store(false, std::memory_order_release);
    return status;
}

void ModbusManager::setMqttManager(MqttManager *mqtt) {
    _mqtt = mqtt;
}

uint8_t ModbusManager::findSlaveIdByDatapointId(const String &dpId) const {
    const ModbusDevice *device = nullptr;
    const ModbusDatapoint *dp = findDatapointById(dpId, &device);
    if (dp && device) {
        return device->slaveId;
    }
    return 0;
}

const ModbusDatapoint *ModbusManager::findDatapointById(const String &dpId, const ModbusDevice **outDevice) const {
    for (const auto &dev: _modbusRoot.devices) {
        for (const auto &dp: dev.datapoints) {
            if (dp.id == dpId) {
                if (outDevice) {
                    *outDevice = &dev;
                }
                return &dp;
            }
        }
    }
    if (outDevice) {
        *outDevice = nullptr;
    }
    return nullptr;
}

String ModbusManager::registersToAscii(const uint16_t *buf, const uint16_t count) {
    String out;
    if (!buf || count == 0) {
        return out;
    }
    out.reserve(count * 2);
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t word = buf[i];
        const char high = static_cast<char>((word >> 8) & 0xFF);
        const char low = static_cast<char>(word & 0xFF);
        if (high != '\0') out += high;
        if (low != '\0') out += low;
    }
    return out;
}

void ModbusManager::publishDatapoint(const ModbusDevice &device, const ModbusDatapoint &dp,
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

    auto &mutableDevice = const_cast<ModbusDevice &>(device);
    if (mutableDevice.homeassistantDiscoveryEnabled) {
        if (!mutableDevice.haAvailabilityOnlinePublished) {
            publishAvailabilityOnline(mutableDevice);
        }
        if (!mutableDevice.haDiscoveryPublished) {
            publishHomeAssistantDiscovery(mutableDevice);
        }
        if (!mutableDevice.haAvailabilityOnlinePublished || !mutableDevice.haDiscoveryPublished) {
            return;
        }
    }

    String topic = buildDatapointTopic(device, dp);
    topic.trim();
    if (!topic.length()) {
        _logger->logWarning("ModbusManager::publishDatapoint - empty topic, skipping publish");
        return;
    }

    if (!_mqtt->mqttPublish(topic.c_str(), payload.c_str())) {
        _logger->logWarning((String("MQTT publish failed for topic ") + topic).c_str());
    } else {
        _logger->logDebug((String("MQTT publish ") + topic + " <= " + payload).c_str());
    }
}

String ModbusManager::buildDatapointTopic(const ModbusDevice &device, const ModbusDatapoint &dp) const {
    String topic = dp.topic;
    topic.trim();
    if (topic.length()) {
        return topic;
    }

    String root = _mqtt->getRootTopic();
    root.trim();

    const String deviceSegment = buildDeviceSegment(device);
    const String datapointSegment = buildDatapointSegment(dp);

    String resolved;
    resolved.reserve(root.length() + deviceSegment.length() + datapointSegment.length() + 2);
    if (root.length()) {
        resolved = root;
        if (!resolved.endsWith("/")) {
            resolved += "/";
        }
        resolved += deviceSegment;
    } else {
        resolved = deviceSegment;
    }
    resolved += "/";
    resolved += datapointSegment;
    return resolved;
}

String ModbusManager::buildAvailabilityTopic(const ModbusDevice &device) const {
    const String deviceSegment = buildDeviceSegment(device);
    String root = _mqtt->getRootTopic();
    root.trim();

    String topic;
    if (root.length()) {
        topic = root;
        if (!topic.endsWith("/")) {
            topic += "/";
        }
        topic += deviceSegment;
    } else {
        topic = deviceSegment;
    }
    topic += "/status";
    return topic;
}

String ModbusManager::buildDeviceSegment(const ModbusDevice &device) {
    String deviceName = device.name;
    deviceName.trim();
    String segment = StringUtils::slugify(deviceName);
    if (!segment.length()) {
        String fallbackId = device.id;
        fallbackId.trim();
        if (!fallbackId.length()) {
            fallbackId = String("device_") + String(device.slaveId);
        }
        segment = StringUtils::slugify(fallbackId);
    }
    if (!segment.length()) {
        segment = String("device");
    }
    return segment;
}

String ModbusManager::buildDatapointSegment(const ModbusDatapoint &dp) {
    String dpName = dp.name;
    dpName.trim();
    String segment = StringUtils::slugify(dpName);
    if (!segment.length()) {
        const int separatorIndex = dp.id.lastIndexOf('.');
        if (separatorIndex >= 0) {
            const auto nextIndex = static_cast<unsigned int>(separatorIndex + 1);
            if (nextIndex < dp.id.length()) {
                segment = StringUtils::slugify(dp.id.substring(nextIndex));
            } else {
                segment = StringUtils::slugify(dp.id);
            }
        } else {
            segment = StringUtils::slugify(dp.id);
        }
    }
    if (!segment.length()) {
        segment = String("datapoint");
    }
    return segment;
}

String ModbusManager::buildFriendlyName(const ModbusDevice &device, const ModbusDatapoint &dp) {
    auto toTitle = [](String value) {
        value.trim();
        if (!value.length()) {
            return value;
        }
        String result;
        result.reserve(value.length() + 4);
        bool newWord = true;
        for (size_t i = 0; i < value.length(); ++i) {
            const auto raw = static_cast<unsigned char>(value[i]);
            if (raw == '_' || raw == '-' || raw == '.') {
                if (result.length() && result[result.length() - 1] != ' ') {
                    result += ' ';
                }
                newWord = true;
                continue;
            }
            if (newWord) {
                result += static_cast<char>(std::toupper(raw));
                newWord = false;
            } else {
                result += static_cast<char>(std::tolower(raw));
            }
        }
        while (result.length() && result[result.length() - 1] == ' ') {
            result.remove(result.length() - 1);
        }
        return result;
    };

    String deviceLabel = toTitle(device.name);
    if (!deviceLabel.length()) {
        deviceLabel = toTitle(device.id);
    }
    if (!deviceLabel.length()) {
        deviceLabel = toTitle(buildDeviceSegment(device));
    }

    String datapointLabel = toTitle(dp.name);
    if (!datapointLabel.length()) {
        datapointLabel = toTitle(dp.id);
    }

    if (deviceLabel.length() && datapointLabel.length()) {
        return deviceLabel + " " + datapointLabel;
    }
    if (deviceLabel.length()) {
        return deviceLabel;
    }
    return datapointLabel;
}

bool ModbusManager::isReadOnlyFunction(const ModbusFunctionType fn) {
    return fn == READ_COIL || fn == READ_DISCRETE || fn == READ_HOLDING || fn == READ_INPUT;
}

void ModbusManager::rebuildWriteSubscriptions() {
    if (!_mqtt) return;

    if (!_writeTopics.empty()) {
        _mqtt->removeSubscriptionHandlers(_writeTopics);
        _writeTopics.clear();
    }

    if (!MqttManager::isMQTTEnabled()) {
        return;
    }

    for (const auto &device: _modbusRoot.devices) {
        if (!device.mqttEnabled) continue;

        for (const auto &dp: device.datapoints) {
            if (isReadOnlyFunction(dp.function)) continue;

            String topic = buildDatapointTopic(device, dp);
            topic.trim();
            if (!topic.length()) {
                _logger->logWarning("ModbusManager::rebuildWriteSubscriptions - empty topic for write datapoint, skipping");
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

void ModbusManager::handleWriteCommand(const String &topic,
                                       const uint8_t slaveId,
                                       const ModbusFunctionType fn,
                                       const uint16_t addr,
                                       const uint8_t numRegs,
                                       const float scale,
                                       const String &payload) {
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
    } else if (fn == WRITE_HOLDING) {
        if (!trimmed.length()) {
            _logger->logWarning("ModbusManager::handleWriteCommand - empty payload for holding register write");
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
        _logger->logWarning("ModbusManager::handleWriteCommand - unsupported function");
        return;
    }

    if (!hasWriteValue) {
        _logger->logWarning(
            (String("ModbusManager::handleWriteCommand - Unable to parse payload for topic [") + topic + "]").c_str());
        return;
    }

    uint16_t outBuf[1]{};
    uint16_t outCount = 0;
    String rxDump;
    const uint8_t status = executeCommand(slaveId,
                                          static_cast<int>(fn),
                                          addr,
                                          numRegs,
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
             ", code=" + String(status) + " (" + statusToString(status) + ")" +
             (rxDump.length() ? String(", rx=") + rxDump : String(""))).c_str());
    }
}

void ModbusManager::handleMqttConnected() {
    if (!_mqtt || !MqttManager::isMQTTEnabled()) {
        return;
    }
    rebuildWriteSubscriptions();
    for (auto &device: _modbusRoot.devices) {
        if (!device.mqttEnabled) {
            continue;
        }
        if (device.homeassistantDiscoveryEnabled) {
            publishAvailabilityOnline(device);
            publishHomeAssistantDiscovery(device);
        }
    }
}

void ModbusManager::handleMqttDisconnected() {
    for (auto &device: _modbusRoot.devices) {
        device.haAvailabilityOnlinePublished = false;
        device.haDiscoveryPublished = false;
    }
}

void ModbusManager::publishAvailabilityOnline(ModbusDevice &device) const {
    if (!device.homeassistantDiscoveryEnabled || !device.mqttEnabled) {
        return;
    }
    if (!MqttManager::isMQTTEnabled() || !_mqtt->isConnected()) {
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

void ModbusManager::publishHomeAssistantDiscovery(ModbusDevice &device) const {
    _logger->logDebug((String("[MQTT][HA] Publishing discovery for device ") + device.id).c_str());
    if (!device.homeassistantDiscoveryEnabled || !device.mqttEnabled) {
        return;
    }

    if (!MqttManager::isMQTTEnabled() || !_mqtt->isConnected()) {
        return;
    }

    const String deviceSegment = buildDeviceSegment(device);
    const String availabilityTopic = buildAvailabilityTopic(device);
    String deviceIdentifier = device.id;
    deviceIdentifier.trim();
    if (!deviceIdentifier.length()) {
        deviceIdentifier = deviceSegment;
    }

    bool anyEligible = false;
    bool anyPublished = false;
    for (const auto &dp: device.datapoints) {
        if (!isReadOnlyFunction(dp.function)) {
            continue;
        }
        anyEligible = true;

        String stateTopic = buildDatapointTopic(device, dp);
        stateTopic.trim();
        if (!stateTopic.length()) {
            continue;
        }

        const String datapointSegment = buildDatapointSegment(dp);
        String discoveryTopic = String("homeassistant/sensor/") + deviceSegment + "/" + datapointSegment + "/config";

        JsonDocument doc;
        doc["name"] = buildFriendlyName(device, dp);
        const String uniqueId = deviceSegment + "_" + datapointSegment;
        doc["unique_id"] = uniqueId;
        doc["object_id"] = uniqueId;
        doc["state_topic"] = stateTopic;
        if (dp.unit.length()) {
            doc["unit_of_measurement"] = dp.unit;
        }
        if (dp.function == READ_HOLDING) {
            doc["state_class"] = "measurement";
        }
        doc["availability_topic"] = availabilityTopic;
        doc["payload_available"] = "online";
        doc["payload_not_available"] = "offline";

        auto deviceObj = doc["device"].to<JsonObject>();
        auto identifiers = deviceObj["identifiers"].to<JsonArray>();
        identifiers.add(deviceIdentifier);
        deviceObj["name"] = device.name.length() ? device.name : deviceSegment;

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

// slugify moved to utils/StringUtils

void ModbusManager::incrementBusErrorCount() const {
    BUS_ERROR_COUNT.fetch_add(1, std::memory_order_relaxed);
    _logger->logDebug(("Total errors: " + String(getBusErrorCount())).c_str());
}

uint32_t ModbusManager::getBusErrorCount() {
    return BUS_ERROR_COUNT.load(std::memory_order_relaxed);
}

void ModbusManager::setModbusEnabled(const bool enabled) {
    BUS_ACTIVE.store(enabled, std::memory_order_release);
}

bool ModbusManager::getBusState() {
    return BUS_ACTIVE.load(std::memory_order_acquire);
}
