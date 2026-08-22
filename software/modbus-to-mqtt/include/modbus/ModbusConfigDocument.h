#ifndef MODBUS_TO_MQTT_MODBUS_CONFIG_DOCUMENT_H
#define MODBUS_TO_MQTT_MODBUS_CONFIG_DOCUMENT_H

#include <cstddef>
#include <cstdint>

#include "modbus/config_structs/ConfigurationRoot.h"

namespace ModbusConfigDocument {

enum class ValidationReason : uint8_t {
    None = 0,
    InvalidJson,
    InvalidScale,
};

struct ValidationError {
    ValidationReason reason{ValidationReason::None};
    size_t deviceIndex{0U};
    size_t datapointIndex{0U};
};

bool parse(const char *json, size_t length, ConfigurationRoot &outConfig,
           ValidationError *validationError = nullptr);

const char *reasonToString(ValidationReason reason);

}  // namespace ModbusConfigDocument

#endif
