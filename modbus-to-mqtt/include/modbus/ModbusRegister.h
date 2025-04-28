#ifndef MBREGISTER_H
#define MBREGISTER_H

#pragma once

#include "ModbusDataType.h"
#include <Arduino.h>
#include "RegisterType.h"

struct ModbusRegister {
    uint16_t address;
    uint8_t numOfRegisters;
    float scale;
    RegisterType registerType;
    ModbusDataType dataType;
    char name[16];
};
#endif //MBREGISTER_H
