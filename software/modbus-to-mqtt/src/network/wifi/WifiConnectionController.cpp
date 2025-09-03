#include "network/wifi/WifiConnectionController.h"

void WifiConnectionController::begin(const String &hostname) {
    Serial.printf("WiFiConnectController::begin(%s) called\n", hostname.c_str());
    _hostname = hostname;
    WiFi.persistent(false);
    WiFiClass::mode(WIFI_STA);
    WiFi.disconnect(true, false);
    _status = {};
    _status.state = WifiConnectionState::Idle;

    WiFi.onEvent([this](WiFiEvent_t e, WiFiEventInfo_t info) {
        this->onEvent(e, info);
    });

    Serial.println("WiFiConnectController::begin() ended\n");
}

void WifiConnectionController::reset() {
    WiFi.disconnect(true, true);
    _status = {};
    _status.state = WifiConnectionState::Idle;
}

void WifiConnectionController::setApChannelIfNeeded(const uint8_t ch) {
        Serial.printf("WiFiConnectController::setApChannelIfNeeded(%d) called\n", ch);
        if (!ch) return;
        wifi_config_t wifi_config{};
        if (esp_wifi_get_config(WIFI_IF_AP, &wifi_config) == ESP_OK) {
            if (wifi_config.ap.channel != ch) {
                wifi_config.ap.channel = ch;
                esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
            }
        }
}

bool WifiConnectionController::connect(const String &ssid, const String &pass, const String &bssidStr,
    const WifiStaticConfig &static_config, const bool save, const uint8_t channel) {

        if (_status.state == WifiConnectionState::Connecting) {
            return false;
        }

        Serial.println("WiFiConnectController::connect called");
        _status = {};
        _status.state = WifiConnectionState::Connecting;
        _status.ssid = ssid;
        _deadline = millis() + _timeoutMs;

        WiFiClass::mode(WIFI_AP_STA);
        WiFi.setAutoReconnect(false);
        setApChannelIfNeeded(channel);

        if (static_config.any()) {
            Serial.println("WiFiConnectController::connect - Using static config");

            IPAddress ip, gw, mask, dns1, dns2;
            if (!ip.fromString(static_config.ip) || !gw.fromString(static_config.gateway) || !mask.fromString(static_config.subnet)) {
                fail("BAD_STATIC_CONFIG");
                return true;
            }
            if (static_config.dns1.length()) {
                (void) dns1.fromString(static_config.dns1);
            }
            if (static_config.dns2.length()) {
                (void) dns2.fromString(static_config.dns2);
            }
            WiFi.config(ip, gw, mask, dns1, dns2);
        } else {
            Serial.println("WiFiConnectController::connect - Using DHCP config");
            WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
        }

        if (_hostname.length()) {
            WiFiClass::setHostname(_hostname.c_str());
        }

        uint8_t bssid[6];
        const bool useBssid = parseBssid(bssidStr, bssid);

        Serial.printf("WiFiConnectController::connect - Wi-Fi Persist 0/1: %d\n", save);
        WiFi.persistent(save);

        const int staChan = channel ? channel : 0;
        const bool ok = useBssid
                            ? WiFi.begin(ssid.c_str(), pass.c_str(), staChan, bssid)
                            : WiFi.begin(ssid.c_str(), pass.c_str(), staChan);
        WiFi.persistent(false);

        if (!ok) {
            fail("BEGIN_FAILED");
            return true;
        }
        _persistRequested = save;
        if (save) {
            _persistSsid = ssid;
            _persistPass = pass;
        }
        Serial.printf("WiFiConnectController::connect - WiFi.begin(%s, ******) returned OK", ssid.c_str());
        Serial.println();
        return true;
}

void WifiConnectionController::loop() {
    if (_status.state == WifiConnectionState::Connecting && millis() > _deadline) {
        WiFi.disconnect(true, false);
        fail("TIMEOUT");
    }
}

WifiStatus WifiConnectionController::getStatus() const {
        return _status;
}

void WifiConnectionController::cancel() {
        if (_status.state == WifiConnectionState::Connecting) {
            WiFi.disconnect(true, false);
            _status.state = WifiConnectionState::Failed;
            _status.reason = "CANCELLED";
        }
}

void WifiConnectionController::onEvent(const arduino_event_id_t event, const arduino_event_info_t &info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("WiFiConnectController::onEvent called with ARDUINO_EVENT_WIFI_STA_CONNECTED");
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
            Serial.println("WiFiConnectController::onEvent - Event: ARDUINO_EVENT_WIFI_STA_GOT_IP");
            _status.state = WifiConnectionState::Connected;
            _status.ip = WiFi.localIP().toString();
            _status.hasIp = true;
            WiFi.setAutoReconnect(true);
            if (_persistRequested && _persistSsid.length()) {
                writePlainCredsToNvs(_persistSsid, _persistPass);
                _persistRequested = false;
                _persistSsid = "";
                _persistPass = "";
            }
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            Serial.println("WiFiConnectController::onEvent called with ARDUINO_EVENT_WIFI_STA_DISCONNECTED");
            String r = "DISCONNECTED";
            if (info.wifi_sta_disconnected.reason == WIFI_REASON_NO_AP_FOUND) r = "NO_AP_FOUND";
            else if (info.wifi_sta_disconnected.reason == WIFI_REASON_AUTH_FAIL) r = "WRONG_PASSWORD";
            else if (info.wifi_sta_disconnected.reason == WIFI_REASON_BEACON_TIMEOUT) r = "BEACON_TIMEOUT";
            else if (info.wifi_sta_disconnected.reason == WIFI_REASON_ASSOC_EXPIRE) r = "ASSOC_EXPIRE";
            else if (info.wifi_sta_disconnected.reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT)
                r = "HANDSHAKE_TIMEOUT";

            if (_status.state == WifiConnectionState::Connecting) {
                fail(r);
            } else {
                _status.state = WifiConnectionState::Disconnected;
                _status.reason = r;
            }
            break;
        }
        default: break;
    }
}

bool WifiConnectionController::parseBssid(const String &s, uint8_t out[6]) {
    if (s.length() != 17) return false;
    int vals[6];
    if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(vals[i]);
    return true;
}

void WifiConnectionController::fail(const String &reason) {
    Serial.printf("WiFiConnectController::fail(%s) called\n", reason.c_str());
    _status.state = WifiConnectionState::Failed;
    _status.reason = reason;
}

void WifiConnectionController::writePlainCredsToNvs(const String &ssid, const String &pass) {
    Serial.printf("WiFiConnectController::writePlainCredsToNvs(%s, ******) called\n", ssid.c_str());
    wifi_config_t cfg{};
    memset(&cfg, 0, sizeof(cfg));
    strncpy(reinterpret_cast<char *>(cfg.sta.ssid), ssid.c_str(), sizeof(cfg.sta.ssid));
    strncpy(reinterpret_cast<char *>(cfg.sta.password), pass.c_str(), sizeof(cfg.sta.password));
    cfg.sta.bssid_set = false; // do not lock to a BSSID
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    Serial.printf("WiFiConnectController::writePlainCredsToNvs(%s, ******) ended\n", ssid.c_str());
}
