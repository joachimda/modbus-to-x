#ifndef MODBUS_TO_MQTT_MBXSERVERHANDLERS_H
#define MODBUS_TO_MQTT_MBXSERVERHANDLERS_H

#include "ESPAsyncWebServer.h"
#include "logging/MemoryLogger.h"
#include "network/NetworkPortal.h"
#include "network/wifi/WifiConnectionController.h"
#include "mqtt/MqttManager.h"

class Logger;
class ModbusManager; // fwd

class MBXServerHandlers {
public:
    static void setPortal(NetworkPortal *portal);

    static void getSsidListAsJson(AsyncWebServerRequest *req);

    static void handlePutModbusConfigBody(AsyncWebServerRequest *req, const uint8_t *data, size_t len, size_t index,
                                          size_t total);

    static void handlePutMqttConfigBody(AsyncWebServerRequest *req, const uint8_t *data, size_t len, size_t index,
                                        size_t total);

    static void handlePutMqttSecretBody(AsyncWebServerRequest *req, const uint8_t *data, size_t len, size_t index,
                                        size_t total);

    static void handleModbusDisable(AsyncWebServerRequest * req, bool state);

    static void handleNetworkReset();

    static void handleWifiConnect(AsyncWebServerRequest *req, WifiConnectionController &wifi, const uint8_t *data,
                                  size_t len, size_t index, size_t total);

    static void handleWifiStatus(AsyncWebServerRequest *req, const WifiConnectionController &wifi);

    static void handleWifiCancel(AsyncWebServerRequest *req, WifiConnectionController &wifi);

    static void handleWifiApOff(AsyncWebServerRequest *req);

    static void getSystemStats(AsyncWebServerRequest *req, const Logger *logger);

    static void setMemoryLogger(MemoryLogger *mem);

    static void handleCaptivePortalRedirect(AsyncWebServerRequest *req);

    static void setMqttManager(MqttManager *mqttManager);

    static MqttManager *getMqttManager();

    static void setModbusManager(ModbusManager *modbusManager);
    static ModbusManager *getModbusManager();

    static void getLogs(AsyncWebServerRequest *req);

    static void handleDeviceReset(const Logger *logger);

    static void handleMqttTestConnection(AsyncWebServerRequest *req);

    /**
     Execute a Modbus test command
     params: devId, dpId, func_code, addr, len
    */
    static void handleModbusExecute(AsyncWebServerRequest *req);

    static void handleOtaFirmwareUpload(AsyncWebServerRequest *r, const String &fn, size_t index,
                                        uint8_t *data, size_t len, bool final, const Logger *logger);

    static void handleOtaFilesystemUpload(AsyncWebServerRequest *r, const String &fn, size_t index,
                                          uint8_t *data, size_t len, bool final, const Logger *logger);

    static void initEventStream(AsyncWebServer *server, const Logger *logger);
    static void pumpEventStream();
};

#endif
