#ifndef MODBUS_TO_MQTT_NETWORKPORTAL_H
#define MODBUS_TO_MQTT_NETWORKPORTAL_H

#include "Logger.h"
#include "SSIDRecord.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_check.h"

class NetworkPortal {
public:
    NetworkPortal(Logger *logger, DNSServer *dnsServer);
    void begin();
    auto getSsidList() -> std::vector<SSIDRecord>;
    IPAddress getAccessPointIp();

    void onScanDone();

private:
    std::vector<SSIDRecord> _ssidList;
    void runPortal();
    Logger * _logger;
    DNSServer * _dns;

    esp_err_t initWiFiStaOnce();

    void setAPMode();

    void configureDnsServer();

    static void startScanIfIdle();

    static uint8_t rssiToSignal(int8_t rssi);
};

#endif