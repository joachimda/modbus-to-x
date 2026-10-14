#ifndef MODBUS_TO_MQTT_MODBUS_CONFIG_DOCUMENT_H
#define MODBUS_TO_MQTT_MODBUS_CONFIG_DOCUMENT_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "modbus/config_structs/ConfigurationRoot.h"

namespace ModbusConfigDocument {

enum class ValidationReason : uint8_t {
    None = 0,
    InvalidJson,
    InvalidScale,
    // FC6 (write single register) combined with a 32-bit or 64-bit data type.
    InvalidRegisterCount,
};

struct ValidationError {
    ValidationReason reason{ValidationReason::None};
    size_t deviceIndex{0U};
    size_t datapointIndex{0U};
};

// Adjustments applied while parsing so that existing configurations keep
// loading. Each one is reported so the loader can log it.
enum class ParseWarning : uint8_t {
    None = 0,
    // numOfRegisters did not match the data type's width and was coerced.
    RegisterCountCoerced,
    // A byte slice was configured on a multi-register type and was ignored.
    RegisterSliceIgnored,
    // A TEXT data type on a register write was treated as uint16.
    TextWriteCoercedToUint16,
    // numOfRegisters exceeded the Modbus maximum of 125 and was clamped.
    RegisterCountClamped,
};

struct ParseNotice {
    ParseWarning warning{ParseWarning::None};
    size_t deviceIndex{0U};
    size_t datapointIndex{0U};
    uint16_t previous{0U};
    uint16_t coerced{0U};
};

bool parse(const char *json, size_t length, ConfigurationRoot &outConfig,
           ValidationError *validationError = nullptr,
           std::vector<ParseNotice> *notices = nullptr);

const char *reasonToString(ValidationReason reason);

// Human-readable explanation of a rejection, for API responses.
const char *reasonDescription(ValidationReason reason);

const char *warningToString(ParseWarning warning);

}  // namespace ModbusConfigDocument

#endif
