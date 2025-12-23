#ifndef MODBUS_TO_MQTT_CONNECTIONCONTROLLER_H
#define MODBUS_TO_MQTT_CONNECTIONCONTROLLER_H
#pragma once
#include "Config.h"
#include <WiFi.h>
#include <Arduino.h>
#include <esp_wifi.h>
#include "WifiConfiguration.h"

class WifiConnectionController {
public:
    void begin(const String &hostname = DEFAULT_HOSTNAME);

    void reset();

    static void setApChannelIfNeeded(uint8_t ch);

    bool connect(const String &ssid, const String &pass, const String &bssidStr,
                 const WifiStaticConfig &static_config, bool save, uint8_t channel);

    void loop();

    WifiStatus getStatus() const;

    void cancel();

private:
    void onEvent(WiFiEvent_t event, const WiFiEventInfo_t &info);

    static bool parseBssid(const String &s, uint8_t out[6]);

    void fail(const String &reason);

    static void writePlainCredsToNvs(const String &ssid, const String &pass);

    WifiStatus _status;
    String _hostname;
    uint32_t _timeoutMs = 35000;
    uint32_t _deadline = 0;
    bool _persistRequested = false;
    String _persistSsid, _persistPass;
};

#endif
