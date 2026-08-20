#ifndef MODBUS_TO_MQTT_CONNECTIONCONTROLLER_H
#define MODBUS_TO_MQTT_CONNECTIONCONTROLLER_H
#pragma once
#include "Config.h"
#include <WiFi.h>
#include <Arduino.h>
#include <esp_wifi.h>
#include <mutex>
#include "ProvisioningAttemptCore.h"
#include "ProvisioningEventGate.h"
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
    using AttemptCore = ProvisioningAttemptCore::Controller<String, WifiStaticConfig>;
    using AttemptToken = AttemptCore::Token;

    struct PendingConnection {
        String ssid;
        String password;
        String bssid;
        WifiStaticConfig staticConfiguration;
        uint8_t channel = 0U;

        void clear() {
            ssid = String{};
            password = String{};
            bssid = String{};
            staticConfiguration = {};
            channel = 0U;
        }
    };

    void onEvent(WiFiEvent_t event, const WiFiEventInfo_t &info);

    void startConnection(AttemptToken token);

    static ProvisioningEventGate::Event toGateEvent(WiFiEvent_t event);

    static bool parseBssid(const String &s, uint8_t out[6]);

    void fail(AttemptToken token, const String &reason);

    static bool persistCredentials(const String &ssid, const String &pass);

    void removeEventHandler();

    mutable std::mutex _operationMutex;
    AttemptCore _attempt;
    ProvisioningEventGate::Gate<AttemptToken> _eventGate;
    PendingConnection _pendingConnection;
    String _hostname;
    uint32_t _timeoutMs = 35000;
    uint32_t _deadline = 0;
    AttemptToken _activeToken = 0U;
    wifi_event_id_t _eventHandlerId = 0U;
    bool _eventHandlerRegistered = false;
};

#endif
