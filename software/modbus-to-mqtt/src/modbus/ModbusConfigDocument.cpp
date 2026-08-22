#include "modbus/ModbusConfigDocument.h"

#include <ArduinoJson.h>

#include <cmath>
#include <utility>

#include "Config.h"
#include "modbus/ModbusMqttWriteCore.h"
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

bool fail(ValidationError *error, const ValidationReason reason,
          const size_t deviceIndex = 0U, const size_t datapointIndex = 0U) {
    if (error != nullptr) {
        error->reason = reason;
        error->deviceIndex = deviceIndex;
        error->datapointIndex = datapointIndex;
    }
    return false;
}

}  // namespace

bool parse(const char *json, const size_t length, ConfigurationRoot &outConfig,
           ValidationError *validationError) {
    if (validationError != nullptr) *validationError = ValidationError();

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
        candidate.bus.baud = bus["baud"] | DEFAULT_MODBUS_BAUD_RATE;
        candidate.bus.serialFormat = String(bus["serialFormat"] | DEFAULT_MODBUS_MODE);
        candidate.bus.enabled = bus["enabled"] | false;
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
                    datapoint.address = static_cast<uint16_t>(point["address"] | 0);
                    datapoint.numOfRegisters = static_cast<uint8_t>(point["numOfRegisters"] | 1);

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
                    if (point["poll_interval_ms"].is<unsigned long>()) {
                        datapoint.pollIntervalMs =
                            static_cast<uint32_t>(point["poll_interval_ms"].as<unsigned long>());
                    } else {
                        datapoint.pollIntervalMs =
                            static_cast<uint32_t>(point["poll_interval"].as<unsigned long>()) * 1000UL;
                    }
                    datapoint.nextDueAtMs = 0U;
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
        case ValidationReason::None: break;
    }
    return "none";
}

}  // namespace ModbusConfigDocument
