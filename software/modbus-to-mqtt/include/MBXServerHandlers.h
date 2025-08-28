#ifndef MODBUS_TO_MQTT_MBXSERVERHANDLERS_H
#define MODBUS_TO_MQTT_MBXSERVERHANDLERS_H

#include "ESPAsyncWebServer.h"
#include <WiFi.h>
#include <SPIFFS.h>
#include "SSIDRecord.h"

class MBXServerHandlers {
public:
    static void handleUpload(AsyncWebServerRequest *r, const String& fn, size_t index,
                             uint8_t *data, size_t len, bool final);
    static void handleNetworkReset();

    static auto getSsidListAsJson(const std::vector<SSIDRecord>& ssidLists) -> String;
};

#endif
