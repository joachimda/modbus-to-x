#include "network/NetworkPortal.h"
#include <algorithm>
#include <atomic>
#include <WiFi.h>

#include "Logger.h"
#include "Config.h"

static constexpr uint32_t AP_STARTUP_DELAY_MS = 500;
static constexpr uint32_t AP_STARTUP_TIMEOUT_MS = 3000;

static constexpr auto DNS_PORT = 53;
static constexpr auto SIGNAL_UPPER_LIM = 100;

static constexpr uint32_t SSID_SCAN_INTERVAL_MS = 10000;
static constexpr uint32_t SCAN_POLL_INTERVAL_MS = 200;
static constexpr uint16_t SCAN_DWELL_MS = 300;

static std::atomic<uint32_t> lastScanStart{0};
static std::atomic<bool> portal_should_run{true};

static std::shared_ptr<const std::vector<WiFiResult>> atomicLoadResults(const std::shared_ptr<const std::vector<WiFiResult>>* p) {
    return std::atomic_load(p);
}
static void atomicStoreResults(std::shared_ptr<const std::vector<WiFiResult>>* p, std::shared_ptr<const std::vector<WiFiResult>> v) {
    std::atomic_store(p, std::move(v));
}

NetworkPortal::NetworkPortal(Logger *logger, DNSServer *dnsServer) : _logger(logger), _dns(dnsServer) {
    atomicStoreResults(&_latestScanResults, std::make_shared<const std::vector<WiFiResult>>());
}

void NetworkPortal::begin() {
    _logger->logInformation("NetworkPortal::begin - AP+STA bring-up");
    setAPMode();

    if (!waitForApIp()) {
        _logger->logWarning("AP IP not ready, continuing");
    }
    configureDnsServer();

    unsigned long lastScanTime = 0;
    unsigned long lastScanPoll = 0;

    // Serve portal
    while (portal_should_run) {
        if (_dns) _dns->processNextRequest();

        if (WiFi.scanComplete() != WIFI_SCAN_RUNNING &&
            (lastScanTime == 0 || millis() - lastScanTime >= SSID_SCAN_INTERVAL_MS)) {
            WiFi.scanNetworks(true, false, true, SCAN_DWELL_MS, 0); // async, no hidden, passive, dwell, all channels
            lastScanTime = millis();
            lastScanStart.store(lastScanTime, std::memory_order_release);
            _logger->logDebug("NetworkPortal::scanNetworksAsync - scan started (running)");
            }

        if (millis() - lastScanPoll >= SCAN_POLL_INTERVAL_MS) {
            lastScanPoll = millis();
            scanNetworksAsync();
        }

        delay(2);
        yield();
    }

}

auto NetworkPortal::rssiToSignal(const int8_t rssi) -> uint8_t {
    int q = 2 * (static_cast<int>(rssi) + SIGNAL_UPPER_LIM);
    q = std::max(q, 0);
    q = std::min(q, SIGNAL_UPPER_LIM);
    return static_cast<uint8_t>(q);
}

void NetworkPortal::setAPMode() const {
    WiFi.persistent(false);

    if (WiFiClass::getMode() != WIFI_MODE_APSTA) {
        WiFiClass::mode(WIFI_MODE_APSTA);
    }

    const IPAddress ap_ip(192, 168, 4, 1);
    const IPAddress ap_gw(192, 168, 4, 1);
    const IPAddress ap_nm(255, 255, 255, 0);
    WiFi.softAPConfig(ap_ip, ap_gw, ap_nm);

    const bool ok = WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS, 1, false, 4);
    _logger->logInformation(ok ? "AP started" : "AP start FAILED");
    delay(AP_STARTUP_DELAY_MS);

    WiFi.setSleep(false);
}

void NetworkPortal::configureDnsServer() const {
    if (!_dns) return;
    const IPAddress apIP = WiFi.softAPIP();
    const bool ok = _dns->start(DNS_PORT, "*", apIP);
    if (!ok) {
        _logger->logWarning("DNS start failed");
    } else {
        _logger->logInformation(("DNS started on 53, redirecting to " + apIP.toString()).c_str());
    }
}

void NetworkPortal::scanNetworksAsync() {

    const int16_t status = WiFi.scanComplete();

    if (status == WIFI_SCAN_RUNNING) {
        return;
    }

    if (status == WIFI_SCAN_FAILED) {
        WiFi.scanDelete();
        return;
    }

    if (status < 0) {
        return;
    }

    const auto results = std::make_shared<std::vector<WiFiResult>>();
    results->reserve(static_cast<size_t>(status));
    for (int16_t i = 0; i < status; ++i) {
        WiFiResult wifiResult;
        wifiResult.duplicate = false;
        WiFi.getNetworkInfo(i,
                            wifiResult.SSID,
                            wifiResult.encryptionType,
                            wifiResult.RSSI,
                            wifiResult.BSSID,
                            wifiResult.channel);
        results->emplace_back(std::move(wifiResult));
    }

    WiFi.scanDelete();

    atomicStoreResults(&_latestScanResults, std::shared_ptr<const std::vector<WiFiResult>>(results));
}

std::shared_ptr<const std::vector<WiFiResult>> NetworkPortal::getLatestScanResultsSnapshot() const {
    return atomicLoadResults(&_latestScanResults);
}


bool NetworkPortal::waitForApIp() {
    const uint32_t start = millis();
    while (millis() - start < AP_STARTUP_TIMEOUT_MS) {
        IPAddress ip = WiFi.softAPIP();
        if (static_cast<uint32_t>(ip) != 0) return true;
        delay(25);
    }
    return false;
}