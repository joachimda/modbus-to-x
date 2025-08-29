#ifndef MODBUS_TO_MQTT_MBXSERVERHANDLERS_H
#define MODBUS_TO_MQTT_MBXSERVERHANDLERS_H

#include "ESPAsyncWebServer.h"
#include "network/wifi/ConnectController.h"
class NetworkPortal;

namespace MBXServerHandlers {
    void setPortal(NetworkPortal* portal);

    void getSsidListAsJson(AsyncWebServerRequest *req);

    void handleUpload(AsyncWebServerRequest *r, const String &fn, size_t index,
                            uint8_t *data, size_t len, bool final);

    void handleNetworkReset();
    void handleWifiConnect(AsyncWebServerRequest* req, WiFiConnectController& wifi, uint8_t* data, size_t len, size_t index, size_t total);
    void handleWifiStatus(AsyncWebServerRequest* req, WiFiConnectController& wifi);
    void handleWifiCancel(AsyncWebServerRequest* req, WiFiConnectController& wifi);
    void handleWifiApOff(AsyncWebServerRequest* req);
}


#endif
