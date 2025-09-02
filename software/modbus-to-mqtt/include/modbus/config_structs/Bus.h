#ifndef MODBUS_TO_MQTT_BUS_H
#define MODBUS_TO_MQTT_BUS_H
#include <WString.h>

struct Bus {
    int baud;
    String serialFormat;
};
#endif //MODBUS_TO_MQTT_BUS_H