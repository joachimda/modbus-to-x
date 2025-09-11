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
    }
    else {
        _logger->logDebug("ModbusManager::loop - INACTIVE Entry");
        IndicatorService::instance().setModbusConnected(false);
    }

}

bool ModbusManager::readModbusDevice(const ModbusDevice &dev) {
    _logger->logDebug(("ModbusManager::readModbusDevice - Reading Device: " + String(dev.name)).c_str());

    node.begin(dev.slaveId, Serial1);
    node.preTransmission(preTransmissionHandler);
    node.postTransmission(postTransmissionHandler);

    uint8_t result;
    bool successOnThisDevice = false;
    for (auto &dp: dev.datapoints) {
        _logger->logDebug(("ModbusManager::readModbusDevice - Sending Command - Func: " + String(dp.function) + String(dp.name)+ "@" + String(dp.address)).c_str());
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
            const float rawData = node.getResponseBuffer(0);
            const float value = rawData * dp.scale;
            auto payload = String(value);
        } else {
            _logger->logError(
                ("Error reading register: " + String(dp.name) + " Error code: " + String(result) + " Raw Data: " + String(rawdata)).c_str());
        }
    }
    return successOnThisDevice;
}

void ModbusManager::preTransmissionHandler() {
    digitalWrite(RS485_DE_PIN, HIGH);
    digitalWrite(RS485_RE_PIN, HIGH);
}

void ModbusManager::postTransmissionHandler() {
    digitalWrite(RS485_DE_PIN, LOW);
    digitalWrite(RS485_RE_PIN, LOW);
}
