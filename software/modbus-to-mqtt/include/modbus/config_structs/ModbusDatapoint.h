#ifndef MBREGISTER_H
#define MBREGISTER_H

#pragma once

#include "ModbusDataType.h"
#include <Arduino.h>
#include "ModbusFunctionType.h"

enum class RegisterSlice : uint8_t {
    Full = 0,
    LowByte,
    HighByte
};

struct ModbusDatapoint {
    String id;
    String name;
    ModbusFunctionType function;
    uint16_t address;
    uint8_t numOfRegisters;
    float scale;
    ModbusDataType dataType;
    String unit;
    String topic;
    RegisterSlice registerSlice{RegisterSlice::Full};
    uint32_t pollIntervalMs{0};
    uint32_t nextDueAtMs{0};
};
#endif
