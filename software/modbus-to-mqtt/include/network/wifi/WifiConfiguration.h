#ifndef MODBUS_TO_MQTT_WIFICONFIGURATION_H
#define MODBUS_TO_MQTT_WIFICONFIGURATION_H
#include "esp_wifi_types.h"

enum class WifiConnectionState : uint8_t {
    Idle,
    Connecting,
    Connected,
    Failed,
    Disconnected
};

struct WifiScanResult {
    String SSID;
    uint8_t encryptionType = WIFI_AUTH_OPEN;
    int32_t RSSI = 0;
    int32_t channel = 0;
    bool duplicate = false;

    bool hasBSSID = false;
    uint8_t BSSID[6] = {0,0,0,0,0,0};

    WifiScanResult() = default;
};

struct WifiStatus {
    WifiConnectionState state = WifiConnectionState::Idle;
    String ssid;
    String ip;
    String reason;
    bool hasIp = false;
};

struct WifiStaticConfig {
    String ip, gateway, subnet, dns1, dns2;
    bool any() const {
        return ip.length() || gateway.length() || subnet.length() || dns1.length() || dns2.length();
    }
};
#endif