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

#include <esp_wifi_types.h>
#include <WString.h>

class WiFiResult {
public:
    String SSID;
    uint8_t encryptionType = WIFI_AUTH_OPEN;
    int32_t RSSI = 0;
    int32_t channel = 0;
    bool duplicate = false;

    bool hasBSSID = false;
    uint8_t BSSID[6] = {0,0,0,0,0,0};

    WiFiResult() = default;
};
#endif
