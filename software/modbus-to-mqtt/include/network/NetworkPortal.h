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

private:
    Logger *_logger;
    DNSServer *_dns;

    static bool waitForApIp();
    void configureDnsServer() const;
    bool initWiFiStaOnce(const std::string &ssid, const std::string &pass, uint32_t timeout_ms) const;
    void scanNetworksAsync();
    static uint8_t rssiToSignal(int8_t rssi);
    void setAPMode() const;

    std::shared_ptr<const std::vector<WiFiResult>> _latestScanResults;
};

#endif
