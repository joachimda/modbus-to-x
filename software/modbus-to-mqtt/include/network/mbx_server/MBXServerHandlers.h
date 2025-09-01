#ifndef MODBUS_TO_MQTT_MBXSERVERHANDLERS_H
#define MODBUS_TO_MQTT_MBXSERVERHANDLERS_H

#include "ESPAsyncWebServer.h"
#include "network/NetworkPortal.h"
#include "network/wifi/ConnectController.h"

class MBXServerHandlers{
public:
    static void setPortal(NetworkPortal* portal);
    static void getSsidListAsJson(AsyncWebServerRequest *req);
    static void handleUpload(AsyncWebServerRequest *r, const String &fn, size_t index,
                            uint8_t *data, size_t len, bool final);

    static void handleNetworkReset();
    static void handleWifiConnect(AsyncWebServerRequest* req, WiFiConnectController& wifi, uint8_t* data, size_t len, size_t index, size_t total);
    static void handleWifiStatus(AsyncWebServerRequest* req, WiFiConnectController& wifi);
    static void handleWifiCancel(AsyncWebServerRequest* req, WiFiConnectController& wifi);
    static void handleWifiApOff(AsyncWebServerRequest* req);

    static void getSystemStats(AsyncWebServerRequest * req, const Logger * logger);
};

#endif
