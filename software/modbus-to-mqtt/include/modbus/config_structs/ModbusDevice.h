#ifndef MODBUS_TO_MQTT_MODBUSDEVICE_H
#define MODBUS_TO_MQTT_MODBUSDEVICE_H
#include <vector>
#include <WString.h>

#include "ModbusDatapoint.h"

struct ModbusDevice {
    String name;
    uint8_t slaveId;
    std::vector<ModbusDatapoint> datapoints;
};

#endif