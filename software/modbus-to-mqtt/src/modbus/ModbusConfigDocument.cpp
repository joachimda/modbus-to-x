#include "modbus/ModbusConfigDocument.h"

#include <ArduinoJson.h>

#include <cmath>
#include <utility>

#include "Config.h"
#include "modbus/ModbusFunctionUtils.h"
#include "modbus/ModbusMqttWriteCore.h"
#include "modbus/ModbusValueCodec.h"
#include "modbus/WavinFrame.h"
#include "modbus/config_structs/ModbusDatapoint.h"
#include "utils/StringUtils.h"

namespace ModbusConfigDocument {
namespace {

ModbusFunctionType parseFunction(const int function) {
    switch (function) {
        case 1: return READ_COIL;
        case 2: return READ_DISCRETE;
        case 3: return READ_HOLDING;
        case 4: return READ_INPUT;
        case 5: return WRITE_COIL;
        case 6: return WRITE_HOLDING;
        case 16: return WRITE_MULTIPLE_HOLDING;
        case WAVIN_READ: return WAVIN_READ;
        case WAVIN_WRITE: return WAVIN_WRITE;
        default: return READ_HOLDING;
    }
}

ModbusDataType parseDataType(const JsonVariant &value) {
    if (value.is<int>()) {
        switch (value.as<int>()) {
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
    String text = value.as<const char *>();
    text.toLowerCase();
    if (text == "text") return TEXT;
    if (text == "int16") return INT16;
    if (text == "int32") return INT32;
    if (text == "int64") return INT64;
    if (text == "uint16") return UINT16;
    if (text == "uint32") return UINT32;
    if (text == "uint64") return UINT64;
    if (text == "float32") return FLOAT32;
    return UINT16;
}

RegisterSlice parseRegisterSlice(const JsonVariant &value) {
    if (value.is<int>()) {
        switch (value.as<int>()) {
            case 1: return RegisterSlice::LowByte;
            case 2: return RegisterSlice::HighByte;
            default: return RegisterSlice::Full;
        }
    }
    if (value.is<const char *>()) {
        String text = value.as<const char *>();
        text.toLowerCase();
        if (text == "low" || text == "low_byte" || text == "lowbyte" || text == "1") {
            return RegisterSlice::LowByte;
        }
        if (text == "high" || text == "high_byte" || text == "highbyte" || text == "2") {
            return RegisterSlice::HighByte;
        }
        if (text == "full" || text == "full_register") return RegisterSlice::Full;
    }
    return RegisterSlice::Full;
}

ModbusWordOrder parseWordOrder(const JsonVariant &value) {
    if (value.is<int>()) {
        return value.as<int>() == 1 ? ModbusWordOrder::Little : ModbusWordOrder::Big;
    }
    if (value.is<const char *>()) {
        String text = value.as<const char *>();
        text.trim();
        text.toLowerCase();
        if (text == "little" || text == "little_endian" || text == "1") {
            return ModbusWordOrder::Little;
        }
    }
    return ModbusWordOrder::Big;
}

bool isRegisterFunction(const ModbusFunctionType function) {
    return function == READ_HOLDING || function == READ_INPUT
           || function == WRITE_HOLDING || function == WRITE_MULTIPLE_HOLDING
           || function == WAVIN_READ || function == WAVIN_WRITE;
}

constexpr long MAX_REGISTER_COUNT = 125L;
constexpr long MAX_WAVIN_FIELD = 255L;

// A Wavin register index, category or page: an integer in 0-255, or absent
// (0). Anything else is rejected rather than read as 0.
bool readWavinField(const JsonVariantConst value, long &out) {
    if (value.isNull()) {
        out = 0L;
        return true;
    }
    if (!value.is<long>()) return false;
    out = value.as<long>();
    return out >= 0L && out <= MAX_WAVIN_FIELD;
}

bool fail(ValidationError *error, const ValidationReason reason,
          const size_t deviceIndex = 0U, const size_t datapointIndex = 0U) {
    if (error != nullptr) {
        error->reason = reason;
        error->deviceIndex = deviceIndex;
        error->datapointIndex = datapointIndex;
    }
    return false;
}

void notice(std::vector<ParseNotice> *notices, const ParseWarning warning,
            const size_t deviceIndex, const size_t datapointIndex,
            const uint16_t previous, const uint16_t coerced) {
    if (notices == nullptr) return;
    ParseNotice entry;
    entry.warning = warning;
    entry.deviceIndex = deviceIndex;
    entry.datapointIndex = datapointIndex;
    entry.previous = previous;
    entry.coerced = coerced;
    notices->push_back(entry);
}

// Makes numOfRegisters, registerSlice and dataType consistent with the
// function code and the data type's register width. FC6 with a
// multi-register type is rejected in strict mode and treated as uint16 in
// lenient mode.
bool applyRegisterShapeRules(ModbusDatapoint &datapoint, const long requestedCount,
                             const size_t deviceIndex, const size_t datapointIndex,
                             const ParseMode mode, ValidationError *error,
                             std::vector<ParseNotice> *notices) {
    long count = requestedCount < 0L ? 0L : requestedCount;
    const long maximum = isWavinFunction(datapoint.function)
                             ? static_cast<long>(WavinFrame::kMaxRegisters)
                             : MAX_REGISTER_COUNT;
    if (count > maximum) {
        notice(notices, ParseWarning::RegisterCountClamped, deviceIndex, datapointIndex,
               static_cast<uint16_t>(count > 0xFFFFL ? 0xFFFFL : count),
               static_cast<uint16_t>(maximum));
        count = maximum;
    }
    datapoint.numOfRegisters = static_cast<uint8_t>(count);
    if (!isRegisterFunction(datapoint.function)) return true;

    const bool registerWrite = ModbusMqttWriteCore::isWritableHoldingFunction(datapoint.function);
    if (datapoint.dataType == TEXT) {
        if (!registerWrite) return true;
        notice(notices, ParseWarning::TextWriteCoercedToUint16, deviceIndex, datapointIndex,
               static_cast<uint16_t>(TEXT), static_cast<uint16_t>(UINT16));
        datapoint.dataType = UINT16;
    }

    uint8_t width = ModbusValueCodec::wordWidth(datapoint.dataType);
    uint8_t coercedCount = datapoint.numOfRegisters;
    // Only FC16 writes an encoded value wider than one register.
    if (registerWrite && datapoint.function != WRITE_MULTIPLE_HOLDING) {
        if (width != 1U) {
            if (mode == ParseMode::Strict) {
                return fail(error, ValidationReason::InvalidRegisterCount, deviceIndex, datapointIndex);
            }
            // Before data types were honoured every FC6 value was encoded as
            // uint16 (0..65535 x scale, negatives rejected, Home Assistant
            // bounds to match). Keeping that encoding means the upgrade
            // changes nothing for a stored datapoint.
            notice(notices, ParseWarning::DataTypeCoercedToUint16, deviceIndex, datapointIndex,
                   static_cast<uint16_t>(datapoint.dataType), static_cast<uint16_t>(UINT16));
            datapoint.dataType = UINT16;
            width = 1U;
        }
        coercedCount = 1U;
    } else if (datapoint.function == WRITE_MULTIPLE_HOLDING) {
        coercedCount = width;
    } else if (datapoint.numOfRegisters < width) {
        coercedCount = width;
    }
    if (coercedCount != datapoint.numOfRegisters) {
        notice(notices, ParseWarning::RegisterCountCoerced, deviceIndex, datapointIndex,
               datapoint.numOfRegisters, coercedCount);
        datapoint.numOfRegisters = coercedCount;
    }
    if (width > 1U && datapoint.registerSlice != RegisterSlice::Full) {
        notice(notices, ParseWarning::RegisterSliceIgnored, deviceIndex, datapointIndex,
               static_cast<uint16_t>(datapoint.registerSlice),
               static_cast<uint16_t>(RegisterSlice::Full));
        datapoint.registerSlice = RegisterSlice::Full;
    }
    return true;
}

}  // namespace

bool parse(const char *json, const size_t length, ConfigurationRoot &outConfig,
           ValidationError *validationError, std::vector<ParseNotice> *notices,
           const ParseMode mode) {
    if (validationError != nullptr) *validationError = ValidationError();
    if (notices != nullptr) notices->clear();

    JsonDocument document;
    if (json == nullptr || deserializeJson(document, json, length)) {
        return fail(validationError, ValidationReason::InvalidJson);
    }

    ConfigurationRoot candidate{};
    const JsonObject bus = document["bus"].as<JsonObject>();
    if (bus.isNull()) {
        candidate.bus.baud = DEFAULT_MODBUS_BAUD_RATE;
        candidate.bus.serialFormat = DEFAULT_MODBUS_MODE;
        candidate.bus.enabled = false;
    } else {
        // A float, a string or a value outside the int range must not fall
        // back to the default: the bus would run at a rate the stored
        // document does not show.
        const JsonVariantConst baud = bus["baud"];
        if (baud.isNull()) {
            candidate.bus.baud = DEFAULT_MODBUS_BAUD_RATE;
        } else if (baud.is<int>()) {
            candidate.bus.baud = baud.as<int>();
        } else {
            return fail(validationError, ValidationReason::InvalidBaudRate);
        }
        candidate.bus.serialFormat = String(bus["serialFormat"] | DEFAULT_MODBUS_MODE);
        candidate.bus.enabled = bus["enabled"] | false;
    }
    // The bus form offers no lower rate, and the Wavin timing is checked
    // against this floor at compile time (WavinClient.cpp).
    if (candidate.bus.baud < static_cast<int>(MODBUS_MIN_BAUD_RATE)) {
        return fail(validationError, ValidationReason::InvalidBaudRate);
    }

    const JsonArray devices = document["devices"].as<JsonArray>();
    if (!devices.isNull()) {
        candidate.devices.reserve(devices.size());
        size_t deviceIndex = 0U;
        for (JsonObject object : devices) {
            ModbusDevice device{};
            device.name = String(object["name"] | "device");
            device.name.trim();
            device.slaveId = static_cast<uint8_t>(object["slaveId"] | 1);
            device.id = String(object["id"] | "");
            device.id.trim();
            if (!device.id.length()) {
                device.id = StringUtils::slugify(device.name);
                if (!device.id.length()) device.id = String("device_") + String(device.slaveId);
            }
            device.mqttEnabled = object["mqttEnabled"] | false;
            device.homeassistantDiscoveryEnabled = object["homeassistantDiscoveryEnabled"] | false;
            device.haAvailabilityOnlinePublished = false;
            device.haDiscoveryPublished = false;

            const JsonArray datapoints = object["dataPoints"].as<JsonArray>();
            if (!datapoints.isNull()) {
                device.datapoints.reserve(datapoints.size());
                size_t datapointIndex = 0U;
                for (JsonObject point : datapoints) {
                    ModbusDatapoint datapoint{};
                    datapoint.id = String(point["id"] | "");
                    datapoint.name = String(point["name"] | "");
                    datapoint.function = parseFunction(point["function"] | 3);
                    long address = point["address"] | 0L;
                    datapoint.address = static_cast<uint16_t>(address);
                    if (isWavinFunction(datapoint.function)) {
                        // The register index, category and page are one byte
                        // each on the wire; truncating would address another
                        // register, and so would reading a value that is not
                        // an integer as 0.
                        long category = 0L;
                        long page = 0L;
                        if (!readWavinField(point["address"], address)
                            || !readWavinField(point["category"], category)
                            || !readWavinField(point["page"], page)) {
                            return fail(validationError, ValidationReason::InvalidWavinAddress,
                                        deviceIndex, datapointIndex);
                        }
                        datapoint.category = static_cast<uint8_t>(category);
                        datapoint.page = static_cast<uint8_t>(page);
                    }
                    const long requestedCount = point["numOfRegisters"] | 1L;

                    const JsonVariant scaleValue = point["scale"];
                    double parsedScale = 1.0;
                    if (!scaleValue.isNull()) {
                        if (!scaleValue.is<double>()) {
                            if (ModbusMqttWriteCore::isWritableHoldingFunction(datapoint.function)) {
                                return fail(validationError, ValidationReason::InvalidScale,
                                            deviceIndex, datapointIndex);
                            }
                        } else {
                            parsedScale = scaleValue.as<double>();
                        }
                    }
                    datapoint.scale = static_cast<float>(parsedScale);
                    if (ModbusMqttWriteCore::isWritableHoldingFunction(datapoint.function)
                        && (!std::isfinite(parsedScale)
                            || !ModbusMqttWriteCore::isValidWritableScale(datapoint.scale))) {
                        return fail(validationError, ValidationReason::InvalidScale,
                                    deviceIndex, datapointIndex);
                    }

                    datapoint.dataType = parseDataType(point["dataType"]);
                    datapoint.unit = String(point["unit"] | "");
                    datapoint.topic = String(point["topic"] | "");
                    datapoint.topic.trim();
                    datapoint.registerSlice = parseRegisterSlice(point["registerSlice"]);
                    datapoint.wordOrder = parseWordOrder(point["wordOrder"]);
                    if (point["poll_interval_ms"].is<unsigned long>()) {
                        datapoint.pollIntervalMs =
                            static_cast<uint32_t>(point["poll_interval_ms"].as<unsigned long>());
                    } else {
                        datapoint.pollIntervalMs =
                            static_cast<uint32_t>(point["poll_interval"].as<unsigned long>()) * 1000UL;
                    }
                    datapoint.nextDueAtMs = 0U;
                    if (!applyRegisterShapeRules(datapoint, requestedCount, deviceIndex, datapointIndex,
                                                 mode, validationError, notices)) {
                        return false;
                    }
                    device.datapoints.push_back(datapoint);
                    ++datapointIndex;
                }
            }
            candidate.devices.push_back(device);
            ++deviceIndex;
        }
    }

    outConfig = std::move(candidate);
    return true;
}

const char *reasonToString(const ValidationReason reason) {
    switch (reason) {
        case ValidationReason::InvalidJson: return "invalid_json";
        case ValidationReason::InvalidScale: return "invalid_scale";
        case ValidationReason::InvalidRegisterCount: return "invalid_register_count";
        case ValidationReason::InvalidWavinAddress: return "invalid_wavin_address";
        case ValidationReason::InvalidBaudRate: return "invalid_baud_rate";
        case ValidationReason::None: break;
    }
    return "none";
}

static_assert(MODBUS_MIN_BAUD_RATE == 1200U, "update the InvalidBaudRate description");

const char *reasonDescription(const ValidationReason reason) {
    switch (reason) {
        case ValidationReason::InvalidJson: return "Configuration is not valid JSON";
        case ValidationReason::InvalidScale:
            return "Writable holding-register scale must be finite and non-zero";
        case ValidationReason::InvalidRegisterCount:
            return "FC6 and Wavin 0x44 write exactly one register; use FC16 for 32-bit and 64-bit data types";
        case ValidationReason::InvalidWavinAddress:
            return "Wavin functions need address (register index), category and page in 0-255";
        case ValidationReason::InvalidBaudRate: return "Bus baud rate must be an integer of at least 1200";
        case ValidationReason::None: break;
    }
    return "";
}

bool locatesDatapoint(const ValidationReason reason) {
    switch (reason) {
        case ValidationReason::InvalidScale:
        case ValidationReason::InvalidRegisterCount:
        case ValidationReason::InvalidWavinAddress:
            return true;
        case ValidationReason::None:
        case ValidationReason::InvalidJson:
        case ValidationReason::InvalidBaudRate:
            break;
    }
    return false;
}

const char *warningToString(const ParseWarning warning) {
    switch (warning) {
        case ParseWarning::RegisterCountCoerced: return "register_count_coerced";
        case ParseWarning::RegisterSliceIgnored: return "register_slice_ignored";
        case ParseWarning::TextWriteCoercedToUint16: return "text_write_coerced_to_uint16";
        case ParseWarning::RegisterCountClamped: return "register_count_clamped";
        case ParseWarning::DataTypeCoercedToUint16: return "data_type_coerced_to_uint16";
        case ParseWarning::None: break;
    }
    return "none";
}

}  // namespace ModbusConfigDocument
