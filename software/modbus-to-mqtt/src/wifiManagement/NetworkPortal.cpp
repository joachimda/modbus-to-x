#include <DNSServer.h>
#include <algorithm>
#include <atomic>
#include "wifiManagement/NetworkPortal.h"
#include "Logger.h"
#include "Config.h"

static const uint32_t ap_startup_delay_ms = 500;
static const auto dns_port = 53;
static const auto SSID_BYTE_LEN = 32;
static const auto SCAN_TIME_MIN = 30;
static const auto SCAN_TIME_MAX = 60;
static const auto SIGNAL_UPPER_LIM = 100;

static const char* TAG = "wifi_scan";
static constexpr uint32_t SSID_SCAN_INTERVAL_MS = 10000;

static void onScanDoneStatic(void* arg, esp_event_base_t base, int32_t id, void* data) {
    reinterpret_cast<NetworkPortal*>(arg)->onScanDone();
}

portMUX_TYPE ssidMux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> scanInProgress{false};
std::atomic<uint32_t> lastScanStart{0};

static std::atomic<bool> portal_should_run{true};

NetworkPortal::NetworkPortal(Logger * logger, DNSServer *dnsServer) : _logger(logger), _dns(dnsServer) {}
void NetworkPortal::begin() {
    _logger->logInformation("NetworkPortal::begin - SET AP STA begin");;
    setAPMode();
    configureDnsServer();
    static esp_event_handler_instance_t hScan{};
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &onScanDoneStatic, this, &hScan));

    startScanIfIdle();
    while (portal_should_run) {
        _dns->processNextRequest();
        const uint32_t now = millis();
        if (!scanInProgress && (now - lastScanStart.load()) >= SSID_SCAN_INTERVAL_MS) {
            startScanIfIdle();
        }
        delay(1);
    }
    _logger->logInformation("NetworkPortal::begin - SET AP STA end. AP Stopped");;
}

void NetworkPortal::startScanIfIdle() {
    wifi_scan_config_t cfg{};
    cfg.show_hidden = true;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min = SCAN_TIME_MIN;
    cfg.scan_time.active.max = SCAN_TIME_MAX;

    if (!scanInProgress.exchange(true)) {
        lastScanStart = millis();
        esp_err_t err = esp_wifi_scan_start(&cfg, false);
        if (err != ESP_OK) {
            scanInProgress = false;
            ESP_LOGE(TAG, "scan_start failed: %s", esp_err_to_name(err));
        }
    }
}

void NetworkPortal::onScanDone() {
    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK) {
        scanInProgress = false;
        return;
    }
    std::vector<wifi_ap_record_t> recs(ap_count);
    if (esp_wifi_scan_get_ap_records(&ap_count, recs.data()) != ESP_OK) {
        scanInProgress = false;
        return;
    }

    std::vector<SSIDRecord> result;
    result.reserve(ap_count);
    for (uint16_t i = 0; i < ap_count; ++i) {
        const auto& ap = recs[i];
        SSIDRecord rec{};
        size_t len = strnlen(reinterpret_cast<const char*>(ap.ssid), SSID_BYTE_LEN);
        rec.ssid.assign(reinterpret_cast<const char*>(ap.ssid), len);
        rec.signal = rssiToSignal(ap.rssi);
        result.push_back(std::move(rec));
    }

    // Swap under a short critical section
    taskENTER_CRITICAL(&ssidMux);
    _ssidList.swap(result);
    taskEXIT_CRITICAL(&ssidMux);

    scanInProgress = false;
}

auto NetworkPortal::rssiToSignal(int8_t rssi) -> uint8_t {
    int q = 2 * (static_cast<int>(rssi) + SIGNAL_UPPER_LIM);
    q = std::max(q, 0); q = std::min(q, SIGNAL_UPPER_LIM);
    return static_cast<uint8_t>(q);
}
void NetworkPortal::setAPMode() {
    ESP_ERROR_CHECK(esp_netif_init());
    (void)esp_event_loop_create_default();

    // Ensure default AP netif exists
    esp_netif_t* netifInstance = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netifInstance == nullptr) {
        ESP_ERROR_CHECK(esp_netif_create_default_wifi_ap() ? ESP_OK : ESP_FAIL);
        netifInstance = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    }

    // Ensure Wi‑Fi is initialized and in APSTA mode
    wifi_mode_t mode{};
    esp_err_t wifiModeResult = esp_wifi_get_mode(&mode);
    if (wifiModeResult == ESP_ERR_WIFI_NOT_INIT) {
        if (initWiFiStaOnce() != ESP_OK) {
            _logger->logError("Failed to init Wi‑Fi");
            return;
        }
        ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));
    }

    if (mode != WIFI_MODE_APSTA) {
        (void)esp_wifi_stop();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    else {
        wifi_bandwidth_t bw{};
        esp_err_t s = esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
        if (s == ESP_ERR_WIFI_NOT_STARTED || s == ESP_ERR_WIFI_NOT_INIT) {
            ESP_ERROR_CHECK(esp_wifi_start());
        }
    }
    wifi_config_t ap_cfg{};
    snprintf(reinterpret_cast<char*>(ap_cfg.ap.ssid), sizeof(ap_cfg.ap.ssid), "%s", DEFAULT_AP_SSID);
    snprintf(reinterpret_cast<char*>(ap_cfg.ap.password), sizeof(ap_cfg.ap.password), "%s", DEFAULT_AP_PASS);
    ap_cfg.ap.ssid_len = 0;
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = (strlen(DEFAULT_AP_PASS) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    delay(ap_startup_delay_ms);
}

void NetworkPortal::configureDnsServer() {
    auto apIpAddress = getAccessPointIp();
    _dns->setErrorReplyCode(DNSReplyCode::NoError);
    if (!_dns->start(dns_port, "*", apIpAddress)) {
        _logger->logError("DNS server failed to start.");
    }

}

auto NetworkPortal::getSsidList() -> std::vector<SSIDRecord> {
    taskENTER_CRITICAL(&ssidMux);
    auto copy = _ssidList;
    taskEXIT_CRITICAL(&ssidMux);
    return copy;
}

auto NetworkPortal::initWiFiStaOnce() -> esp_err_t
{
    // 1) NVS (required by Wi-Fi)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));

    // 2) Netif & event loop
    ESP_ERROR_CHECK(esp_netif_init());
    // ignore "already created"
    (void)esp_event_loop_create_default();

    // 3) Default AP and STA netifs (create only once; harmless if already exist)
    if (esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") == nullptr) {
        ESP_ERROR_CHECK(esp_netif_create_default_wifi_ap() ? ESP_OK : ESP_FAIL);
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr) {
        ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_FAIL);
    }

    // 4) Wi-Fi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Ensure we’re in a scan‑capable mode while keeping AP available
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Optional: avoid PS impacting active scan timings
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    return esp_wifi_start();
}

auto NetworkPortal::getAccessPointIp() -> IPAddress {
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif != nullptr) {
        esp_netif_ip_info_t ip_info{};
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            return {ip_info.ip.addr};
        }
        _logger->logWarning("Failed to get AP IP info");
        return nullptr;
    }
    _logger->logWarning("AP netif not found (WIFI_AP_DEF)");
    return nullptr;
}
