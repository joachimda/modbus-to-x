#include "Config.h"
#include "modbus/ModbusManager.h"
#include "storage/ConfigFs.h"

#include <cmath>
#include <vector>

#include "mqtt/MqttManager.h"
#include "services/IndicatorService.h"
#include "modbus/ModbusConfigLoader.h"
#include "modbus/ModbusPollScheduler.h"

ModbusManager::ModbusManager(Logger *logger)
    : _bus(logger),
      _mqttBridge(logger, this),
      _logger(logger) {
}

bool ModbusManager::begin() {
    if (loadConfiguration()) {
        _bus.begin(_modbusRoot.bus);
        _bus.setActive(_modbusRoot.bus.enabled);
        _logger->logInformation(_modbusRoot.bus.enabled
            ? "ModbusManager::begin - RS485 bus is ACTIVE"
            : "ModbusManager::begin - RS485 bus is INACTIVE");
        return _modbusRoot.bus.enabled;
    }
    _bus.setActive(false);
    _logger->logInformation("ModbusManager::begin - RS485 bus is INACTIVE");
    return false;
}

bool ModbusManager::loadConfiguration() {
    const bool ok = ModbusConfigLoader::loadConfiguration(_logger, ConfigFs::kModbusConfigFile, _modbusRoot);
    if (!ok) {
        return false;
    }
    _mqttBridge.onConfigurationLoaded(_modbusRoot);

    _logger->logInformation((String("Loaded config: ") + String(_modbusRoot.devices.size()) + " devices; baud " +
                             String(_modbusRoot.bus.baud) + ", format " + _modbusRoot.bus.serialFormat).c_str());
    return true;
}

void ModbusManager::loop() {
    const bool mqttConnectedNow = (_mqtt != nullptr) && _mqtt->isConnected();
    _mqttBridge.onConnectionState(mqttConnectedNow, _mqttConnectedLastLoop, _modbusRoot);
    _mqttConnectedLastLoop = mqttConnectedNow;

    if (!_bus.isActive()) {
        IndicatorService::instance().setModbusConnected(false);
        return;
    }

    bool anySuccess = false;
    bool anyAttempted = false;

    const uint32_t now = millis();
    for (auto &dev: _modbusRoot.devices) {
        _dueScratch.clear();
        const size_t dueCount = ModbusPollScheduler::collectDueReadDatapoints(dev, now, _dueScratch);
        if (dueCount == 0) continue;
        anyAttempted = true;
        anySuccess = readModbusDevice(dev, _dueScratch, now) || anySuccess;
    }
    if (anyAttempted) {
        IndicatorService::instance().setModbusConnected(anySuccess);
    }
    if (!anySuccess) {
    }
}

bool ModbusManager::readModbusDevice(ModbusDevice &dev,
                                     const std::vector<ModbusDatapoint *> &dueDatapoints,
                                     const uint32_t now) {
    auto guard = _bus.acquire();
    if (!guard) {
        return false;
    }

    ModbusMaster &node = _bus.node();
    Stream &busStream = _bus.stream();
    node.begin(dev.slaveId, busStream);

    uint8_t result;
    bool successOnThisDevice = false;
    for (auto *dpPtr: dueDatapoints) {
        if (!dpPtr) continue;
        auto &dp = *dpPtr;
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
            case WRITE_COIL:
            case WRITE_HOLDING:
                ModbusPollScheduler::scheduleNext(dp, now);
                continue;
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
            _mqttBridge.publishDatapoint(dev, dp, payload);
        } else {
            // Dump captured RX bytes for diagnostics
            const String rxDump = _bus.dumpRx();
            _logger->logError((String("Modbus ERR - ") + String(dev.name) +
                               ": func=" + functionToString(dp.function) +
                               ", addr=" + String(dp.address) +
                               ", regs=" + String(dp.numOfRegisters) +
                               ", slave=" + String(dev.slaveId) +
                               ", bus=" + String(_modbusRoot.bus.baud) + "," + _modbusRoot.bus.serialFormat +
                               ", code=" + String(result) + " (" + statusToString(result) + ")" + rxDump).c_str());
            incrementBusErrorCount();
        }
        ModbusPollScheduler::scheduleNext(dp, now);
    }
    return successOnThisDevice;
}

bool ModbusManager::reconfigureFromFile() {
    _logger->logInformation("ModbusManager::reconfigureFromFile - begin");
    // Stop regular loop polling
    _bus.setActive(false);
    // Wait briefly if a read is in progress
    for (int i = 0; i < 50; ++i) {
        if (!_bus.isBusy()) break;
        delay(5);
    }

    const bool ok = loadConfiguration();
    if (ok) {
        _bus.begin(_modbusRoot.bus);
        _bus.setActive(_modbusRoot.bus.enabled);
        _logger->logInformation(_modbusRoot.bus.enabled
            ? "ModbusManager::reconfigureFromFile - applied and active"
            : "ModbusManager::reconfigureFromFile - applied and inactive");
    } else {
        _bus.setActive(false);
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
        case WRITE_MULTIPLE_HOLDING: return "FC16-WRITE_MULTIPLE_HOLDING";
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

    _logger->logDebug("Execute called");
    outCount = 0;
    rxDump = "";
    const bool expectedWrite = (function == 5 || function == 6 || function == 16);
    if (expectedWrite && !hasWriteValue) {
        _logger->logError("hasWriteValue is false");
        return ModbusMaster::ku8MBIllegalDataValue;
    }
    const bool expectedRead = (function >= 1 && function <= 4);
    if (!expectedRead && !expectedWrite) {
        _logger->logError("function out of range");
        return ModbusMaster::ku8MBIllegalFunction;
    }
    const uint16_t effectiveLen = (function == 16) ? 1 : len;

    if (!_bus.isInitialized()) {
        if (_modbusRoot.bus.baud == 0) {
            _modbusRoot.bus.baud = DEFAULT_MODBUS_BAUD_RATE;
            _modbusRoot.bus.serialFormat = DEFAULT_MODBUS_MODE;
        }
        _bus.begin(_modbusRoot.bus);
    }

    auto guard = _bus.acquire();
    if (!guard) {
        return 0xE4; // busy
    }

    _bus.enableCapture(true);

    ModbusMaster &node = _bus.node();
    Stream &busStream = _bus.stream();
    node.begin(slaveId, busStream);

    uint8_t status;
    switch (function) {
        case 1: status = node.readCoils(addr, effectiveLen);
            break;
        case 2: status = node.readDiscreteInputs(addr, effectiveLen);
            break;
        case 3: status = node.readHoldingRegisters(addr, effectiveLen);
            break;
        case 4: status = node.readInputRegisters(addr, effectiveLen);
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
        case 16: {
            _logger->logDebug(("Execute F16 on addr: " + String(addr) + " with Data: " + String(writeValue)).c_str());
            node.setTransmitBuffer(0, writeValue);
            status = node.writeMultipleRegisters(addr, effectiveLen);
            break;
        }
        default:
            status = ModbusMaster::ku8MBIllegalFunction;
            break;
    }

    if (status == ModbusMaster::ku8MBSuccess && expectedRead && outBuf && outBufCap > 0) {
        const uint16_t n = (effectiveLen < outBufCap) ? effectiveLen : outBufCap;
        for (uint16_t i = 0; i < n; ++i) {
            outBuf[i] = node.getResponseBuffer(i);
        }
        outCount = n;
    }

    rxDump = _bus.dumpRx();

    if (status != ModbusMaster::ku8MBSuccess) {
        incrementBusErrorCount();
    }

    return status;
}

void ModbusManager::setMqttManager(MqttManager *mqtt) {
    _mqtt = mqtt;
    _mqttBridge.setMqttManager(mqtt);
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

void ModbusManager::incrementBusErrorCount() {
    _bus.incrementError();
    _logger->logDebug(("Total errors: " + String(getBusErrorCount())).c_str());
}

uint32_t ModbusManager::getBusErrorCount() {
    return ModbusBus::getErrorCount();
}

void ModbusManager::setModbusEnabled(const bool enabled) {
    ModbusBus::setEnabled(enabled);
}

bool ModbusManager::getBusState() {
    return ModbusBus::isEnabled();
}
