#include "storage/ConfigFs.h"

#include "modbus/ModbusConfigLoader.h"
#include "modbus/ModbusValueCodec.h"
#include "Config.h"

namespace {

// Data-type notices carry ModbusDataType values; print their names.
String noticeValueToString(const ModbusConfigDocument::ParseWarning warning, const uint16_t value) {
    using ModbusConfigDocument::ParseWarning;
    if (warning == ParseWarning::TextWriteCoercedToUint16
        || warning == ParseWarning::DataTypeCoercedToUint16) {
        return String(ModbusValueCodec::dataTypeName(static_cast<ModbusDataType>(value)));
    }
    return String(value);
}

}  // namespace

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

    // A stored document must keep loading: one datapoint an earlier firmware
    // accepted (FC6 with a 32/64-bit type) keeps its old uint16 encoding and
    // is logged rather than stopping Modbus on every device.
    return parseConfiguration(logger, json.c_str(), json.length(), outConfig, nullptr,
                              ModbusConfigDocument::ParseMode::Lenient);
}

bool ModbusConfigLoader::parseConfiguration(Logger *logger, const char *json,
                                            const size_t length,
                                            ConfigurationRoot &outConfig,
                                            ModbusConfigDocument::ValidationError *validationError,
                                            const ModbusConfigDocument::ParseMode mode) {
    ModbusConfigDocument::ValidationError localError;
    auto *error = validationError != nullptr ? validationError : &localError;
    std::vector<ModbusConfigDocument::ParseNotice> notices;
    if (ModbusConfigDocument::parse(json, length, outConfig, error, &notices, mode)) {
        if (logger != nullptr) {
            for (const auto &notice : notices) {
                logger->logWarning((String("Modbus configuration adjusted: ")
                                    + ModbusConfigDocument::warningToString(notice.warning)
                                    + ", deviceIndex=" + String(notice.deviceIndex)
                                    + ", datapointIndex=" + String(notice.datapointIndex)
                                    + ", previous=" + noticeValueToString(notice.warning, notice.previous)
                                    + ", coerced=" + noticeValueToString(notice.warning, notice.coerced)).c_str());
            }
        }
        return true;
    }

    if (logger != nullptr) {
        String message = String("Modbus configuration rejected: reason=")
                         + ModbusConfigDocument::reasonToString(error->reason);
        if (error->reason != ModbusConfigDocument::ValidationReason::InvalidJson) {
            message += String(", deviceIndex=") + String(error->deviceIndex)
                       + ", datapointIndex=" + String(error->datapointIndex);
        }
        logger->logError(message.c_str());
    }
    return false;
}
