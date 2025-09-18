#include "Config.h"
#include "Modbus/ModbusManager.h"

#include <atomic>
#include <map>
#include <SPIFFS.h>

#include "ArduinoJson.h"
#include "services/IndicatorService.h"

static std::atomic<bool> BUS_ACTIVE{false};
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

class TeeStream : public Stream {
public:
    TeeStream(Stream &inner, Logger *logger) : _inner(inner), _logger(logger) {
    }

    void enableCapture(const bool en) {
        _capture = en;
        if (en) {
            _bufLen = 0;
            _sawFirstByte = false;
        }
    }

    String dumpHex() const {
        if (_bufLen == 0) return {""};
        String s;
        s.reserve(_bufLen * 3 + 8);
        s = " RX=";
        for (size_t i = 0; i < _bufLen; ++i) {
            if (_buf[i] < 16) s += "0";
            s += String(_buf[i], HEX);
            if (i + 1 < _bufLen) s += " ";
        }
        return s;
    }

    int available() override {
#if RS485_DROP_LEADING_ZERO
        if (_capture && !_sawFirstByte) {
            // Non-blocking purge of leading 0x00 bytes
            while (_inner.available() > 0) {
                const int pk = _inner.peek();
                if (pk != 0x00) break;
                // Drop zero
                (void) _inner.read();
                // Do not record in the capture buffer and do not set _sawFirstByte
            }
        }
#endif
        return _inner.available();
    }

    int read() override {
        int b = _inner.read();
#if RS485_DROP_LEADING_ZERO
        if (_capture && !_sawFirstByte) {
            // Drop leading 0x00 bytes; if the next byte isn't immediately available,
            // wait briefly for it to arrive to avoid returning a spurious 0x00.
            uint32_t waited = 0;
            int drops = 0;
            while (b == 0x00 && drops < 8) {
                if (_inner.available() > 0) {
                    b = _inner.read();
                    drops++;
                    continue;
                }
                if (waited >= RS485_FIRSTBYTE_WAIT_US) {
                    // Do not propagate a zero as the first byte; report "no data"
                    return -1;
                }
                delayMicroseconds(20);
                waited += 20;
            }
        }
#endif
        if (_capture && b >= 0 && _bufLen < sizeof(_buf)) {
            _buf[_bufLen++] = static_cast<uint8_t>(b);
            if (!_sawFirstByte && b != 0x00) {
                _sawFirstByte = true;
            }
        } else if (_capture && !_sawFirstByte && b == 0x00) {
            // Explicitly ignore zero as the first byte for state tracking
        }
        return b;
    }

    int peek() override {
#if RS485_DROP_LEADING_ZERO
        if (_capture && !_sawFirstByte) {
            // Purge any leading zeros so peek exposes the first non-zero
            while (_inner.available() > 0) {
                const int pk = _inner.peek();
                if (pk != 0x00) break;
                (void) _inner.read(); // drop zero
            }
        }
#endif
        return _inner.peek();
    }

    void flush() override { _inner.flush(); }
    size_t write(const uint8_t ch) override {
        return _inner.write(ch);
    }
    size_t write(const uint8_t *buffer, const size_t size) override {
        return _inner.write(buffer, size);
    }

private:
    Stream &_inner;
    Logger *_logger;
    bool _capture{false};
    uint8_t _buf[64]{};
    size_t _bufLen{0};
    bool _sawFirstByte{false};
};

static TeeStream *g_teeSerial1 = nullptr;
static std::atomic<bool> g_modbusBusy{false};

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
    auto path = "/conf/config.json";
    if (!SPIFFS.exists(path)) {
        _logger->logDebug("Configuration file not found '/conf/config.json'");
        // fallback to defaults
        _modbusRoot.bus.baud = DEFAULT_MODBUS_BAUD_RATE;
        _modbusRoot.bus.serialFormat = DEFAULT_MODBUS_MODE;
        _modbusRoot.devices.clear();
        return false;
    }

    _logger->logDebug("Found configuration file '/conf/config.json'");

    File f = SPIFFS.open(path, FILE_READ);
    if (!f) {
        _logger->logError("ModbusManager::loadConfiguration - Failed to open /conf/config.json");
        return false;
    }
    String json = f.readString();
    f.close();

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, json);
    if (err) {
        _logger->logError((String("ModbusManager::loadConfiguration - JSON parse error: ") + err.c_str()).c_str());
        return false;
    }

    auto parseFunction = [](const int fn) -> ModbusFunctionType {
        switch (fn) {
            case 1: return READ_COIL;
            case 2: return READ_DISCRETE;
            case 3: return READ_HOLDING;
            case 4: return READ_INPUT;
            case 5: return WRITE_COIL;
            case 6: return WRITE_HOLDING;
            default: return READ_HOLDING;
        }
    };
    auto parseDataType = [](const JsonVariant &v) -> ModbusDataType {
        if (v.is<int>()) {
            const int n = v.as<int>();
            switch (n) {
                case 1: return TEXT;
                case 2: return INT16;
                case 3: return INT32;
                case 4: return INT64;
                case 5: return UINT16;
                case 6: return UINT32;
                case 7: return UINT64;
                case 8: return FLOAT32;
                default: return UINT16;
            }
        }
        String s = v.as<const char *>();
        s.toLowerCase();
        if (s == "text") {
            return TEXT;
        }
        if (s == "int16") {
            return INT16;
        }
        if (s == "int32") {
            return INT32;
        }
        if (s == "int64") {
            return INT64;
        }
        if (s == "uint16") {
            return UINT16;
        }
        if (s == "uint32") {
            return UINT32;
        }
        if (s == "uint64") {
            return UINT64;
        }
        if (s == "float32") {
            return FLOAT32;
        }
        return UINT16;
    };

    // bus
    const JsonObject bus = doc["bus"].as<JsonObject>();
    if (bus.isNull()) {
        _logger->logWarning("ModbusManager::loadConfiguration - missing 'bus' object; using defaults");
        _modbusRoot.bus.baud = DEFAULT_MODBUS_BAUD_RATE;
        _modbusRoot.bus.serialFormat = DEFAULT_MODBUS_MODE;
    } else {
        _modbusRoot.bus.baud = bus["baud"] | DEFAULT_MODBUS_BAUD_RATE;
        _modbusRoot.bus.serialFormat = String(bus["serialFormat"] | DEFAULT_MODBUS_MODE);
    }

    // devices
    _modbusRoot.devices.clear();
    const JsonArray devs = doc["devices"].as<JsonArray>();
    if (!devs.isNull()) {
        _modbusRoot.devices.reserve(devs.size());
        for (JsonObject d: devs) {
            ModbusDevice dev{};
            dev.name = String(d["name"] | "device");
            dev.slaveId = static_cast<uint8_t>(d["slaveId"] | 1);

            const JsonArray dps = d["dataPoints"].as<JsonArray>();
            if (!dps.isNull()) {
                dev.datapoints.reserve(dps.size());
                for (JsonObject p: dps) {
                    ModbusDatapoint dp{};
                    dp.id = String(p["id"] | "");
                    dp.name = String(p["name"] | "");
                    dp.function = parseFunction(p["function"] | 3);
                    dp.address = static_cast<uint16_t>(p["address"] | 0);
                    dp.numOfRegisters = static_cast<uint8_t>(p["numOfRegisters"] | 1);
                    dp.scale = static_cast<float>(p["scale"] | 1.0);
                    dp.dataType = parseDataType(p["dataType"]);
                    dp.unit = String(p["unit"] | "");
                    // Optional per-datapoint poll interval (seconds in JSON) -> ms in runtime
                    // Accept both poll_interval (seconds) and poll_interval_ms (milliseconds) if provided
                    if (p["poll_interval_ms"].is<unsigned long>()) {
                        const uint32_t ms = static_cast<uint32_t>(p["poll_interval_ms"].as<unsigned long>());
                        dp.pollIntervalMs = ms;
                    } else {
                        const uint32_t sec = static_cast<uint32_t>(p["poll_interval"].as<unsigned long>());
                        dp.pollIntervalMs = sec * 1000UL;
                    }
                    dp.nextDueAtMs = 0;
                    dev.datapoints.push_back(dp);
                }
            }
            _modbusRoot.devices.push_back(dev);
        }
    }

    _logger->logInformation((String("Loaded config: ") + String(_modbusRoot.devices.size()) + " devices; baud " +
                             String(_modbusRoot.bus.baud) + ", format " + _modbusRoot.bus.serialFormat).c_str());
    return true;
}

void ModbusManager::initializeWiring() const {
    _logger->logDebug("ModbusManager::initialize - Entry");

    pinMode(RS485_DE_PIN, OUTPUT);
    pinMode(RS485_RE_PIN, OUTPUT);

    digitalWrite(RS485_DE_PIN, LOW);
    digitalWrite(RS485_RE_PIN, LOW);

    Serial1.begin(_modbusRoot.bus.baud,
                  communicationModes.at(_modbusRoot.bus.serialFormat),
                  RX2, TX2);

    if (!g_teeSerial1) {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        g_teeSerial1 = new TeeStream(Serial1, _logger);
    }

    _logger->logDebug("ModbusManager::initialize - Exit");
}

void ModbusManager::loop() {
    if (BUS_ACTIVE.load(std::memory_order_acquire)) {
        bool anySuccess = false;
        bool anyAttempted = false;

        const uint32_t now = millis();
        for (auto &dev: _modbusRoot.devices) {
            // Check if any datapoint on this device is due
            bool due = false;
            for (const auto &dp: dev.datapoints) {
                if (dp.pollIntervalMs == 0 || now >= dp.nextDueAtMs) { due = true; break; }
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
    } else {
        _logger->logDebug("ModbusManager::loop - INACTIVE Entry");
        IndicatorService::instance().setModbusConnected(false);
    }
}

bool ModbusManager::readModbusDevice(const ModbusDevice &dev) {
    // mark bus busy to avoid concurrent adhoc commands
    bool was = false;
    if (!g_modbusBusy.compare_exchange_strong(was, true, std::memory_order_acq_rel)) {
        return false;
    }
    _logger->logDebug(
        ("ModbusManager::readModbusDevice - Reading Device: " + String(dev.name) + " SlaveId: " + String(dev.slaveId)).
        c_str());
    float rawData = -99;

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
        // Skip if not due yet
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
            String topic = dp.name;
            rawData = node.getResponseBuffer(0);
            const float value = rawData * dp.scale;
            auto payload = String(value);
            _logger->logDebug(("Modbus OK - " + String(dev.name) + ": " + String(dp.name) +
                                     " = " + payload + " (raw=" + String(rawData) + ")").c_str());
        } else {
            // Dump captured RX bytes for diagnostics
            String rxDump = (g_teeSerial1 ? g_teeSerial1->dumpHex() : String(""));
            _logger->logError((String("Modbus ERR - ") + String(dev.name) +
                               ": func=" + functionToString(dp.function) +
                               ", addr=" + String(dp.address) +
                               ", regs=" + String(dp.numOfRegisters) +
                               ", slave=" + String(dev.slaveId) +
                               ", bus=" + String(_modbusRoot.bus.baud) + "," + _modbusRoot.bus.serialFormat +
                               ", code=" + String(result) + " (" + statusToString(result) + ")" + rxDump).c_str());
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
    // Enable TX, disable RX; stop capture
    digitalWrite(RS485_DE_PIN, HIGH);
    digitalWrite(RS485_RE_PIN, HIGH);
    if (g_teeSerial1) g_teeSerial1->enableCapture(false);
    // Allow transceiver to settle before sending
    delayMicroseconds(RS485_DIR_GUARD_US);
}

void ModbusManager::postTransmissionHandler() {
    Serial1.flush();
    // Disable TX, enable RX; start capture
    digitalWrite(RS485_DE_PIN, LOW);
    digitalWrite(RS485_RE_PIN, LOW);
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
        BUS_ACTIVE.store(true, std::memory_order_release);
        _logger->logInformation("ModbusManager::reconfigureFromFile - applied and active");
    } else {
        BUS_ACTIVE.store(false, std::memory_order_release);
        _logger->logError("ModbusManager::reconfigureFromFile - failed to load config; bus inactive");
    }
    return ok;
}

auto ModbusManager::statusToString(uint8_t code) -> const char * {
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

auto ModbusManager::functionToString(ModbusFunctionType fn) -> const char * {
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

uint8_t ModbusManager::executeCommand(uint8_t slaveId,
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

    // guard against concurrent access
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

    uint8_t status = ModbusMaster::ku8MBIllegalFunction;
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

    g_modbusBusy.store(false, std::memory_order_release);
    return status;
}

uint8_t ModbusManager::findSlaveIdByDatapointId(const String &dpId) const {
    for (const auto &dev: _modbusRoot.devices) {
        for (const auto &dp: dev.datapoints) {
            if (dp.id == dpId) return dev.slaveId;
        }
    }
    return 0;
}
