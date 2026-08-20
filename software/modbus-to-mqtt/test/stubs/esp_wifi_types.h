#ifndef MODBUS_TO_MQTT_TEST_ESP_WIFI_TYPES_H
#define MODBUS_TO_MQTT_TEST_ESP_WIFI_TYPES_H

#include <cstdint>

enum wifi_auth_mode_t : std::uint8_t {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WPA2_ENTERPRISE,
    WIFI_AUTH_WPA3_PSK,
};

#endif
