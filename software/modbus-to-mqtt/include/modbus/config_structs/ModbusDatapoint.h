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
};
#endif //MBREGISTER_H
