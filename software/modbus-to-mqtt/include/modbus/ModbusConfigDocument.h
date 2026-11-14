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
    // A Wavin function (0x43/0x44) with an index, category or page outside
    // 0..255.
    InvalidWavinAddress,
    // A bus baud rate below MODBUS_MIN_BAUD_RATE, or one that is not an
    // integer.
    InvalidBaudRate,
    // poll_interval_ms above ModbusPollScheduler::MAX_POLL_INTERVAL_MS,
    // poll_interval above MAX_LEGACY_POLL_INTERVAL_SECONDS, or either one
    // negative, fractional or not a number.
    InvalidPollInterval,
};

struct ValidationError {
    ValidationReason reason{ValidationReason::None};
    size_t deviceIndex{0U};
    size_t datapointIndex{0U};
};

// How FC6 combined with a 32-bit or 64-bit data type, and a polling interval
// outside the accepted range, are handled; every other shape problem is
// repaired in both modes.
enum class ParseMode : uint8_t {
    // Reject the document (API submissions: the caller can fix it).
    Strict = 0,
    // Repair the datapoint and report a notice, so a configuration saved by
    // an earlier firmware keeps loading. FC6 with a wide data type is
    // treated as uint16 (the encoding every FC6 value had before data types
    // were honoured). A polling interval is brought into range: a negative
    // number becomes 0, a fraction is rounded down, a number above the
    // maximum becomes the maximum, and a value that is not a number counts
    // as absent.
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
    // poll_interval_ms or poll_interval was outside the accepted range and
    // was repaired (lenient mode only). `coerced` is the interval in use, in
    // milliseconds; `previous` is not set.
    PollIntervalRepaired,
};

struct ParseNotice {
    ParseWarning warning{ParseWarning::None};
    size_t deviceIndex{0U};
    size_t datapointIndex{0U};
    uint32_t previous{0U};
    uint32_t coerced{0U};
};

bool parse(const char *json, size_t length, ConfigurationRoot &outConfig,
           ValidationError *validationError = nullptr,
           std::vector<ParseNotice> *notices = nullptr,
           ParseMode mode = ParseMode::Strict);

const char *reasonToString(ValidationReason reason);

// Human-readable explanation of a rejection, for API responses.
const char *reasonDescription(ValidationReason reason);

// Whether a rejection for this reason points at a datapoint, so that the
// deviceIndex and datapointIndex of the ValidationError mean something.
bool locatesDatapoint(ValidationReason reason);

const char *warningToString(ParseWarning warning);

}  // namespace ModbusConfigDocument

#endif
