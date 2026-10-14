#ifndef MODBUS_FUNCTION_UTILS_H
#define MODBUS_FUNCTION_UTILS_H

#include "modbus/config_structs/ModbusFunctionType.h"

inline bool isReadOnlyFunction(const ModbusFunctionType fn) {
    return fn == READ_COIL || fn == READ_DISCRETE || fn == READ_HOLDING || fn == READ_INPUT
           || fn == WAVIN_READ;
}

inline bool isWriteFunction(const ModbusFunctionType fn) {
    return fn == WRITE_COIL || fn == WRITE_HOLDING || fn == WRITE_MULTIPLE_HOLDING
           || fn == WAVIN_WRITE;
}

// Wavin AHC 9000 functions: sent as raw frames rather than through
// ModbusMaster, and addressed by category/index/page. The *Code variants take
// a raw function number (HTTP/MQTT input) that may not name a function.
inline bool isWavinFunctionCode(const int code) {
    return code == WAVIN_READ || code == WAVIN_WRITE;
}

inline bool isWavinFunction(const ModbusFunctionType fn) {
    return isWavinFunctionCode(static_cast<int>(fn));
}

// Writes of exactly one 16-bit register: FC6 and Wavin 0x44.
inline bool isSingleRegisterWriteCode(const int code) {
    return code == WRITE_HOLDING || code == WAVIN_WRITE;
}

inline bool isSingleRegisterWrite(const ModbusFunctionType fn) {
    return isSingleRegisterWriteCode(static_cast<int>(fn));
}

// Coil and discrete-input reads: the response carries packed bits, so the
// datapoint's data type does not apply.
inline bool isBitFunction(const ModbusFunctionType fn) {
    return fn == READ_COIL || fn == READ_DISCRETE;
}

#endif
