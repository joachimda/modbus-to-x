#ifndef MODBUS_FUNCTION_UTILS_H
#define MODBUS_FUNCTION_UTILS_H

#include "modbus/config_structs/ModbusFunctionType.h"

inline bool isReadOnlyFunction(const ModbusFunctionType fn) {
    return fn == READ_COIL || fn == READ_DISCRETE || fn == READ_HOLDING || fn == READ_INPUT;
}

inline bool isWriteFunction(const ModbusFunctionType fn) {
    return fn == WRITE_COIL || fn == WRITE_HOLDING || fn == WRITE_MULTIPLE_HOLDING;
}

#endif
