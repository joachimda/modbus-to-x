/**************************************************************
    Originally part of WiFiManager for the ESP8266/Arduino platform
   (https://github.com/tzapu/WiFiManager)
   Built by AlexT https://github.com/tzapu
   Ported to Async Web Server by https://github.com/alanswx
   Licensed under MIT license

   Modified by joachimda (2025)
   Changes include: simplification to accommodate custom use
 **************************************************************/

#ifndef MODBUS_TO_MQTT_WIFIRESULT_H
#define MODBUS_TO_MQTT_WIFIRESULT_H

#include <cstdint>
#include <WString.h>

class WiFiResult {
public:
    bool duplicate;
    String SSID;
    uint8_t encryptionType;
    int32_t RSSI;
    uint8_t *BSSID;
    int32_t channel;

    WiFiResult() = default;
};
#endif
