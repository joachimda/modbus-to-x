#ifndef MODBUS_TO_MQTT_MBXSERVERHANDLERS_H
#define MODBUS_TO_MQTT_MBXSERVERHANDLERS_H

#include "ESPAsyncWebServer.h"
class NetworkPortal;

namespace MBXServerHandlers {
    // Call this once to provide access to the NetworkPortal
    void setPortal(NetworkPortal* portal);

    // Already used by your route setup
    void getSsidListAsJson(AsyncWebServerRequest *req);

    void handleUpload(AsyncWebServerRequest *r, const String &fn, size_t index,
                            uint8_t *data, size_t len, bool final);

    void handleNetworkReset();
}


#endif
