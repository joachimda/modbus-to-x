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

// How FC6 combined with a 32-bit or 64-bit data type is handled; every other
// shape problem is repaired in both modes.
enum class ParseMode : uint8_t {
    // Reject the document (API submissions: the caller can fix it).
    Strict = 0,
    // Treat the data type as uint16 (the encoding every FC6 value had before
    // data types were honoured) and report a notice, so a configuration
    // saved by an earlier firmware keeps loading and behaving as before.
    Lenient,
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
    // FC6 with a 32-bit or 64-bit data type was treated as uint16 (lenient
    // mode only).
    DataTypeCoercedToUint16,
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
           std::vector<ParseNotice> *notices = nullptr,
           ParseMode mode = ParseMode::Strict);

const char *reasonToString(ValidationReason reason);

// Human-readable explanation of a rejection, for API responses.
const char *reasonDescription(ValidationReason reason);

const char *warningToString(ParseWarning warning);

}  // namespace ModbusConfigDocument

#endif
