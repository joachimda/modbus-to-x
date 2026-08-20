#ifndef MODBUS_TO_MQTT_NETWORKPORTAL_H
#define MODBUS_TO_MQTT_NETWORKPORTAL_H

#include <DNSServer.h>
#include <atomic>
#include <functional>
#include <memory>

#include "Logger.h"
#include "wifi/WifiConfiguration.h"

class NetworkPortal {
public:
    NetworkPortal(Logger *logger, DNSServer *dnsServer);

    void begin(const std::function<void()> &loopCallback = {});

    std::shared_ptr<const std::vector<WifiScanResult>> getLatestScanResultsSnapshot() const;

    void suspendScanning(bool on);
private:
    Logger *_logger;
    DNSServer *_dns;

    static bool waitForApIp();
    void configureDnsServer() const;
    void scanNetworksAsync();
    static uint8_t rssiToSignal(int8_t rssi);
    std::atomic<bool> _scanSuspended{false};
    void setAPMode() const;

    std::shared_ptr<const std::vector<WifiScanResult>> _latestScanResults;
};

#endif
