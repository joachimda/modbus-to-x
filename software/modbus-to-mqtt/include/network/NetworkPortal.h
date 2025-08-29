#ifndef MODBUS_TO_MQTT_NETWORKPORTAL_H
#define MODBUS_TO_MQTT_NETWORKPORTAL_H

#include <DNSServer.h>
#include <memory>

#include "Logger.h"
#include "wifi/WifiResult.h"

class NetworkPortal {
public:
    NetworkPortal(Logger *logger, DNSServer *dnsServer);

    void begin();

    std::shared_ptr<const std::vector<WiFiResult>> getLatestScanResultsSnapshot() const;

    static void stop();
    void suspendScanning(bool on);
private:
    Logger *_logger;
    DNSServer *_dns;

    static bool waitForApIp();
    void configureDnsServer() const;
    void scanNetworksAsync();
    static uint8_t rssiToSignal(int8_t rssi);
    bool _scanSuspended = false;
    void setAPMode() const;

    std::shared_ptr<const std::vector<WiFiResult>> _latestScanResults;
};

#endif
