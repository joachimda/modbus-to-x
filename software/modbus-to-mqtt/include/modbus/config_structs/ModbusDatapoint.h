#ifndef MBREGISTER_H
#define MBREGISTER_H

#pragma once

#include "ModbusDataType.h"
#include <Arduino.h>
#include "ModbusFunctionType.h"

struct ModbusDatapoint {
    String id;
    String name;
    ModbusFunctionType function;
    uint16_t address;
    uint8_t numOfRegisters;
    float scale;
    ModbusDataType dataType;
    String unit;
    // Optional poll interval in milliseconds (0 = every loop)
    uint32_t pollIntervalMs{0};
    // Runtime scheduling: next time this datapoint is due (millis())
    uint32_t nextDueAtMs{0};
};
#endif
