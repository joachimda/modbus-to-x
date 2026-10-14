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
// ModbusMaster, and addressed by category/index/page.
inline bool isWavinFunction(const ModbusFunctionType fn) {
    return fn == WAVIN_READ || fn == WAVIN_WRITE;
}

inline bool isWavinFunctionCode(const int code) {
    return code == WAVIN_READ || code == WAVIN_WRITE;
}

// Coil and discrete-input reads: the response carries packed bits, so the
// datapoint's data type does not apply.
inline bool isBitFunction(const ModbusFunctionType fn) {
    return fn == READ_COIL || fn == READ_DISCRETE;
}

#endif
