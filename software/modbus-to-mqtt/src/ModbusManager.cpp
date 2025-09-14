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

// Lightweight wrapper around Stream to capture RX bytes for diagnostics
class TeeStream : public Stream {
public:
    TeeStream(Stream &inner, Logger *logger) : _inner(inner), _logger(logger) {}
    void enableCapture(bool en) { _capture = en; if (en) { _bufLen = 0; _sawFirstByte = false; } }
    String dumpHex() {
        if (_bufLen == 0) return String("");
        String s; s.reserve(_bufLen * 3 + 8);
        s = " RX=";
        for (size_t i = 0; i < _bufLen; ++i) {
            if (_buf[i] < 16) s += "0";
            s += String(_buf[i], HEX);
            if (i + 1 < _bufLen) s += " ";
        }
        return s;
    }
    // Stream interface
    int available() override { return _inner.available(); }
    int read() override {
        int b = _inner.read();
#if RS485_DROP_LEADING_ZERO
        if (_capture && !_sawFirstByte) {
            // Drop leading 0x00 bytes; if next byte isn't immediately available,
            // wait briefly for it to arrive to avoid returning a spurious 0x00.
            uint32_t waited = 0;
            int drops = 0;
            while (b == 0x00 && drops < 8) {
                // If more bytes are available, consume next immediately
                if (_inner.available() > 0) {
                    b = _inner.read();
                    drops++;
                    continue;
                }
                // Else, wait a short time for the real first byte
                if (waited >= RS485_FIRSTBYTE_WAIT_US) {
                    break; // give up; return 0x00
                }
                delayMicroseconds(20);
                waited += 20;
            }
        }
#endif
        if (_capture && b >= 0 && _bufLen < sizeof(_buf)) {
            _buf[_bufLen++] = static_cast<uint8_t>(b);
            _sawFirstByte = true;
        }
        return b;
    }
    int peek() override { return _inner.peek(); }
    void flush() override { _inner.flush(); }
    size_t write(uint8_t ch) override { return _inner.write(ch); }
    size_t write(const uint8_t *buffer, size_t size) override { return _inner.write(buffer, size); }
private:
    Stream &_inner;
    Logger *_logger;
    bool _capture{false};
    uint8_t _buf[64]{};
    size_t _bufLen{0};
    bool _sawFirstByte{false};
};

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
        if (s == "text") return TEXT;
        if (s == "int16") return INT16;
        if (s == "int32") return INT32;
        if (s == "int64") return INT64;
        if (s == "uint16") return UINT16;
        if (s == "uint32") return UINT32;
        if (s == "uint64") return UINT64;
        if (s == "float32") return FLOAT32;
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
        _logger->logDebug("ModbusManager::loop - ACTIVE Entry");
        bool anySuccess = false;
        _logger->logDebug(("ModbusManager::loop - Found devices: " + String(_modbusRoot.devices.size())).c_str());

        for (auto &dev: _modbusRoot.devices) {
            anySuccess = readModbusDevice(dev) || anySuccess;
        }
        IndicatorService::instance().setModbusConnected(anySuccess);
        if (!anySuccess && !_scanAttempted) {
            _scanAttempted = true;
            _logger->logInformation("ModbusManager::loop - No successful reads; scanning slave IDs 1..16 (Fn=04, Addr=2)");
            // Try a safe probe: Input Register (FC04) at address 2, 1 register
            scanSlaveIds(1, 16, READ_INPUT, 2, 1);
        }
    }
    else {
        _logger->logDebug("ModbusManager::loop - INACTIVE Entry");
        IndicatorService::instance().setModbusConnected(false);
    }

}

bool ModbusManager::readModbusDevice(const ModbusDevice &dev) {
    _logger->logDebug(("ModbusManager::readModbusDevice - Reading Device: " + String(dev.name) + " SlaveId: " + String(dev.slaveId)).c_str());
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
    for (auto &dp: dev.datapoints) {
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
            _logger->logInformation(("Modbus OK - " + String(dev.name) + ": " + String(dp.name) +
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
    }
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
    // Disable TX, enable RX; start capture
    digitalWrite(RS485_DE_PIN, LOW);
    digitalWrite(RS485_RE_PIN, LOW);
    if (g_teeSerial1) g_teeSerial1->enableCapture(true);
    // Small guard so RX is ready before the slave replies
    delayMicroseconds(RS485_DIR_GUARD_US);
}

const char *ModbusManager::statusToString(uint8_t code) {
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
        default: return "Unknown";
    }
}

const char *ModbusManager::functionToString(ModbusFunctionType fn) {
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

void ModbusManager::scanSlaveIds(uint8_t startId, uint8_t endId,
                                 ModbusFunctionType fn, uint16_t address, uint8_t numRegs) {
    if (startId < 1) startId = 1;
    if (endId > 247) endId = 247;
    if (endId < startId) return;

    node.preTransmission(preTransmissionHandler);
    node.postTransmission(postTransmissionHandler);

    for (uint8_t id = startId; id <= endId; ++id) {
        node.begin(id, Serial1);
        uint8_t res = 0xFF;
        switch (fn) {
            case READ_COIL:
                res = node.readCoils(address, numRegs);
                break;
            case READ_DISCRETE:
                res = node.readDiscreteInputs(address, numRegs);
                break;
            case READ_HOLDING:
                res = node.readHoldingRegisters(address, numRegs);
                break;
            case READ_INPUT:
            default:
                res = node.readInputRegisters(address, numRegs);
                break;
        }

        if (res == ModbusMaster::ku8MBSuccess) {
            uint16_t val = node.getResponseBuffer(0);
            _logger->logInformation((String("Scan: slave ") + String(id) + " responded OK (" +
                                     functionToString(fn) + ", addr=" + String(address) +
                                     ", val=" + String(val) + ")").c_str());
        } else if (res >= 0x01 && res <= 0x04) {
            _logger->logInformation((String("Scan: slave ") + String(id) +
                                     " responded with exception " + statusToString(res) +
                                     " (" + functionToString(fn) + ", addr=" + String(address) + ")").c_str());
        } else {
            _logger->logDebug((String("Scan: slave ") + String(id) +
                               " no response (" + statusToString(res) + ")").c_str());
        }
        delay(50);
    }
}
