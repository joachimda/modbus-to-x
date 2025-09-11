#ifndef MODBUS_TO_MQTT_MODBUSROOT_H
#define MODBUS_TO_MQTT_MODBUSROOT_H
#include "Bus.h"
#include "ModbusDevice.h"

struct ConfigurationRoot {
    Bus bus;
    std::vector<ModbusDevice> devices;
};
#endif

