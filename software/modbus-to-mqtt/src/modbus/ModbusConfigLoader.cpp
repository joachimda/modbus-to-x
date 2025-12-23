#include "storage/ConfigFs.h"
#include "ArduinoJson.h"

#include "modbus/ModbusConfigLoader.h"
#include "Config.h"
#include "modbus/config_structs/ModbusDatapoint.h"
#include "utils/StringUtils.h"

bool ModbusConfigLoader::loadConfiguration(Logger *logger, const char *path, ConfigurationRoot &outConfig) {
    if (!path || !*path) path = ConfigFs::kModbusConfigFile;

    if (!ConfigFS.exists(path)) {
        if (logger) {
            logger->logDebug(
                (String("Configuration file not found '") + String(ConfigFs::kBasePath) + path + "'").c_str());
        }
        // fallback to defaults
        outConfig.bus.baud = DEFAULT_MODBUS_BAUD_RATE;
        outConfig.bus.serialFormat = DEFAULT_MODBUS_MODE;
        outConfig.devices.clear();
        return false;
    }

    if (logger) {
        logger->logDebug((String("Found configuration file '") + String(ConfigFs::kBasePath) + path + "'").c_str());
    }

    File f = ConfigFS.open(path, FILE_READ);
    if (!f) {
        if (logger) {
            logger->logError(
                (String("ModbusConfigLoader::loadConfiguration - Failed to open ") +
                 String(ConfigFs::kBasePath) + path).c_str());
        }
        return false;
    }
    String json = f.readString();
    f.close();

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, json);
    if (err) {
        if (logger) logger->logError((String("ModbusConfigLoader::loadConfiguration - JSON parse error: ") + err.c_str()).c_str());
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
            case 16: return WRITE_MULTIPLE_HOLDING;
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
    auto parseRegisterSlice = [](const JsonVariant &v) -> RegisterSlice {
        if (v.is<int>()) {
            switch (v.as<int>()) {
                case 1: return RegisterSlice::LowByte;
                case 2: return RegisterSlice::HighByte;
                default: return RegisterSlice::Full;
            }
        }
        if (v.is<const char *>()) {
            String s = v.as<const char *>();
            s.toLowerCase();
            if (s == "low" || s == "low_byte" || s == "lowbyte" || s == "1") return RegisterSlice::LowByte;
            if (s == "high" || s == "high_byte" || s == "highbyte" || s == "2") return RegisterSlice::HighByte;
            if (s == "full" || s == "full_register") return RegisterSlice::Full;
        }
        return RegisterSlice::Full;
    };

    // bus
    const JsonObject bus = doc["bus"].as<JsonObject>();
    if (bus.isNull()) {
        if (logger) logger->logWarning("ModbusConfigLoader::loadConfiguration - missing 'bus' object; using defaults");
        outConfig.bus.baud = DEFAULT_MODBUS_BAUD_RATE;
        outConfig.bus.serialFormat = DEFAULT_MODBUS_MODE;
        outConfig.bus.enabled = false;
    } else {
        outConfig.bus.baud = bus["baud"] | DEFAULT_MODBUS_BAUD_RATE;
        outConfig.bus.serialFormat = String(bus["serialFormat"] | DEFAULT_MODBUS_MODE);
        outConfig.bus.enabled = bus["enabled"] | false;
    }

    // devices
    outConfig.devices.clear();
    const JsonArray devs = doc["devices"].as<JsonArray>();
    if (!devs.isNull()) {
        outConfig.devices.reserve(devs.size());
        for (JsonObject d : devs) {
            ModbusDevice dev{};
            dev.name = String(d["name"] | "device");
            dev.name.trim();
            dev.slaveId = static_cast<uint8_t>(d["slaveId"] | 1);
            dev.id = String(d["id"] | "");
            dev.id.trim();
            if (dev.id.isEmpty()) {
                dev.id = StringUtils::slugify(dev.name);
                if (dev.id.isEmpty()) {
                    dev.id = String("device_") + String(dev.slaveId);
                }
            }
            dev.mqttEnabled = d["mqttEnabled"] | false;
            dev.homeassistantDiscoveryEnabled = d["homeassistantDiscoveryEnabled"] | false;
            dev.haAvailabilityOnlinePublished = false;
            dev.haDiscoveryPublished = false;

            const JsonArray dps = d["dataPoints"].as<JsonArray>();
            if (!dps.isNull()) {
                dev.datapoints.reserve(dps.size());
                for (JsonObject p : dps) {
                    ModbusDatapoint dp{};
                    dp.id = String(p["id"] | "");
                    dp.name = String(p["name"] | "");
                    dp.function = parseFunction(p["function"] | 3);
                    dp.address = static_cast<uint16_t>(p["address"] | 0);
                    dp.numOfRegisters = static_cast<uint8_t>(p["numOfRegisters"] | 1);
                    dp.scale = static_cast<float>(p["scale"] | 1.0);
                    dp.dataType = parseDataType(p["dataType"]);
                    dp.unit = String(p["unit"] | "");
                    dp.topic = String(p["topic"] | "");
                    dp.topic.trim();
                    dp.registerSlice = parseRegisterSlice(p["registerSlice"]);
                    // Optional per-datapoint poll interval (seconds in JSON) -> ms in runtime
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
            outConfig.devices.push_back(dev);
        }
    }
    return true;
}
