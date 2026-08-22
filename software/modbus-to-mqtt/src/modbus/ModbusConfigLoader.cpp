#include "storage/ConfigFs.h"

#include "modbus/ModbusConfigLoader.h"
#include "Config.h"

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

    return parseConfiguration(logger, json.c_str(), json.length(), outConfig);
}

bool ModbusConfigLoader::parseConfiguration(Logger *logger, const char *json,
                                            const size_t length,
                                            ConfigurationRoot &outConfig,
                                            ModbusConfigDocument::ValidationError *validationError) {
    ModbusConfigDocument::ValidationError localError;
    auto *error = validationError != nullptr ? validationError : &localError;
    if (ModbusConfigDocument::parse(json, length, outConfig, error)) return true;

    if (logger != nullptr) {
        String message = String("Modbus configuration rejected: reason=")
                         + ModbusConfigDocument::reasonToString(error->reason);
        if (error->reason == ModbusConfigDocument::ValidationReason::InvalidScale) {
            message += String(", deviceIndex=") + String(error->deviceIndex)
                       + ", datapointIndex=" + String(error->datapointIndex);
        }
        logger->logError(message.c_str());
    }
    return false;
}
