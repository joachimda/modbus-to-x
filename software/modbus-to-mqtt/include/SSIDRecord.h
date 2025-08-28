
#ifndef MODBUS_TO_MQTT_SSIDRECORD_H
#define MODBUS_TO_MQTT_SSIDRECORD_H

#include <cstdint>

struct SSIDRecord {
    std::string ssid{};
    uint8_t signal{};
};
#endif
