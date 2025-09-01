#ifndef MODBUS_TO_MQTT_CONNECTCONTROLLER_H
#define MODBUS_TO_MQTT_CONNECTCONTROLLER_H
#pragma once
#include <WiFi.h>
#include <Arduino.h>
#include <esp_wifi.h>

struct WifiStaticCfg {
    String ip, gateway, subnet, dns1, dns2;

    bool any() const {
        return ip.length() || gateway.length() || subnet.length() || dns1.length() || dns2.length();
    }
};

enum class WifiConnState : uint8_t { Idle, Connecting, Connected, Failed, Disconnected };

struct WifiStatus {
    WifiConnState state = WifiConnState::Idle;
    String ssid;
    String ip;
    String reason; // "WRONG_PASSWORD", "NO_AP_FOUND", "TIMEOUT", "CANCELLED", ...
    bool hasIp = false;
};

class WiFiConnectController {
public:
    void begin(const String &hostname = "modbus-to-x") {
        Serial.printf("WiFiConnectController::begin(%s) called\n", hostname.c_str());
        _hostname = hostname;
        WiFi.persistent(false);
        WiFiClass::mode(WIFI_STA);
        WiFi.disconnect(true, false);
        _status = {};
        _status.state = WifiConnState::Idle;

        WiFi.onEvent([this](WiFiEvent_t e, WiFiEventInfo_t info) {
            this->onEvent(e, info);
        });
        Serial.println("WiFiConnectController::begin() ended\n");
    }

    void reset() {
        WiFi.disconnect(true, true);
        _status = {};
        _status.state = WifiConnState::Idle;
    }

    static void setApChannelIfNeeded(uint8_t ch) {
        Serial.printf("WiFiConnectController::setApChannelIfNeeded(%d) called\n", ch);
        if (!ch) return;
        wifi_config_t apcfg{};
        if (esp_wifi_get_config(WIFI_IF_AP, &apcfg) == ESP_OK) {
            if (apcfg.ap.channel != ch) {
                apcfg.ap.channel = ch;
                esp_wifi_set_config(WIFI_IF_AP, &apcfg);
            }
        }
    }

    bool connect(const String &ssid, const String &pass, const String &bssidStr,
                 const WifiStaticCfg &st, bool save, uint8_t channel) {
        if (_status.state == WifiConnState::Connecting) return false;
        Serial.println("WiFiConnectController::connect called");
        _status = {};
        _status.state = WifiConnState::Connecting;
        _status.ssid = ssid;
        _deadline = millis() + _timeoutMs;

        // Ensure AP+STA mode stays active
        WiFiClass::mode(WIFI_AP_STA);
        WiFi.setAutoReconnect(false);
        setApChannelIfNeeded(channel);

        if (st.any()) {
            Serial.println("WiFiConnectController::connect - Using static config");

            IPAddress ip, gw, mask, dns1, dns2;
            if (!ip.fromString(st.ip) || !gw.fromString(st.gateway) || !mask.fromString(st.subnet)) {
                fail("BAD_STATIC_CONFIG");
                return true; // status is already Failed; the API will return it
            }
            if (st.dns1.length()) { (void) dns1.fromString(st.dns1); } // leave 0.0.0.0 if parse fails
            if (st.dns2.length()) { (void) dns2.fromString(st.dns2); } // leave 0.0.0.0 if parse fails
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

    void loop() {
        if (_status.state == WifiConnState::Connecting && millis() > _deadline) {
            WiFi.disconnect(true, false);
            fail("TIMEOUT");
        }
    }

    WifiStatus status() const { return _status; }

    void cancel() {
        if (_status.state == WifiConnState::Connecting) {
            WiFi.disconnect(true, false);
            _status.state = WifiConnState::Failed;
            _status.reason = "CANCELLED";
        }
    }

private:
    void onEvent(const WiFiEvent_t event, const WiFiEventInfo_t & info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_CONNECTED:
                Serial.println("WiFiConnectController::onEvent called with ARDUINO_EVENT_WIFI_STA_CONNECTED");
                break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
                Serial.println("WiFiConnectController::onEvent - Event: ARDUINO_EVENT_WIFI_STA_GOT_IP");
                _status.state = WifiConnState::Connected;
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

                if (_status.state == WifiConnState::Connecting) {
                    fail(r);
                } else {
                    _status.state = WifiConnState::Disconnected;
                    _status.reason = r;
                }
                break;
            }
            default: break;
        }
    }

    static bool parseBssid(const String &s, uint8_t out[6]) {
        if (s.length() != 17) return false;
        int vals[6];
        if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) {
            return false;
        }
        for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(vals[i]);
        return true;
    }

    void fail(const String &reason) {
        Serial.printf("WiFiConnectController::fail(%s) called\n", reason.c_str());
        _status.state = WifiConnState::Failed;
        _status.reason = reason;
    }

    static void writePlainCredsToNvs(const String &ssid, const String &pass) {
        Serial.printf("WiFiConnectController::writePlainCredsToNvs(%s, ******) called\n", ssid.c_str());
        wifi_config_t cfg{};
        memset(&cfg, 0, sizeof(cfg));
        strncpy(reinterpret_cast<char *>(cfg.sta.ssid), ssid.c_str(), sizeof(cfg.sta.ssid));
        strncpy(reinterpret_cast<char *>(cfg.sta.password), pass.c_str(), sizeof(cfg.sta.password));
        cfg.sta.bssid_set = false; // do NOT lock to a BSSID
        esp_wifi_set_storage(WIFI_STORAGE_FLASH);
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        Serial.printf("WiFiConnectController::writePlainCredsToNvs(%s, ******) ended\n", ssid.c_str());
    }

private:
    WifiStatus _status;
    String _hostname;
    uint32_t _timeoutMs = 35000;
    uint32_t _deadline = 0;
    bool _persistRequested = false;
    String _persistSsid, _persistPass;
};

#endif
