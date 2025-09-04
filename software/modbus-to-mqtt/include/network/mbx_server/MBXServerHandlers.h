#ifndef MODBUS_TO_MQTT_MBXSERVERHANDLERS_H
#define MODBUS_TO_MQTT_MBXSERVERHANDLERS_H

#include "ESPAsyncWebServer.h"
#include "network/NetworkPortal.h"
#include "network/wifi/WifiConnectionController.h"
#include "MemoryLogger.h"
#include "services/OtaService.h"
#include "commlink/CommLink.h"

class MBXServerHandlers{
public:
    static void setPortal(NetworkPortal* portal);
    static void getSsidListAsJson(AsyncWebServerRequest *req);
    static void handleUpload(AsyncWebServerRequest *r, const String &fn, size_t index,
                            const uint8_t *data, size_t len, bool final);
    static void handlePutModbusConfigBody(AsyncWebServerRequest* req, const uint8_t* data, size_t len, size_t index, size_t total);
    static void handlePutMqttConfigBody(AsyncWebServerRequest* req, const uint8_t* data, size_t len, size_t index, size_t total);
    static void handlePutMqttSecretBody(AsyncWebServerRequest* req, const uint8_t* data, size_t len, size_t index, size_t total);


    static void handleNetworkReset();
    static void handleWifiConnect(AsyncWebServerRequest* req, WifiConnectionController& wifi, const uint8_t* data, size_t len, size_t index, size_t total);
    static void handleWifiStatus(AsyncWebServerRequest* req, const WifiConnectionController& wifi);
    static void handleWifiCancel(AsyncWebServerRequest* req, WifiConnectionController& wifi);
    static void handleWifiApOff(AsyncWebServerRequest* req);

    static void getSystemStats(AsyncWebServerRequest * req, const Logger * logger);
    static void setMemoryLogger(MemoryLogger* mem);
    static void setCommLink(CommLink* link);
    static void getLogs(AsyncWebServerRequest* req);

    static void handleDeviceReset(const Logger *logger);

    // MQTT
    static void handleMqttTestConnection(AsyncWebServerRequest *req);

    // OTA upload handlers
    static void handleOtaFirmwareUpload(AsyncWebServerRequest *r, const String &fn, size_t index,
                                        uint8_t *data, size_t len, bool final, const Logger *logger);
    static void handleOtaFilesystemUpload(AsyncWebServerRequest *r, const String &fn, size_t index,
                                         uint8_t *data, size_t len, bool final, const Logger *logger);
};

#endif
