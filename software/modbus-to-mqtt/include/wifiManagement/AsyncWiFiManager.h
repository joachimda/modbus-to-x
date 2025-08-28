/**************************************************************
    Originally part of WiFiManager for the ESP8266/Arduino platform
   (https://github.com/tzapu/WiFiManager)
   Built by AlexT https://github.com/tzapu
   Ported to Async Web Server by https://github.com/alanswx
   Licensed under MIT license

   Modified by joachimda (2025)
   Changes include: simplification to accommodate custom use
 **************************************************************/

#ifndef MODBUS_TO_MQTT_ASYNCWIFIMANAGER_H
#define MODBUS_TO_MQTT_ASYNCWIFIMANAGER_H

#include <DNSServer.h>
#include <WiFiType.h>
#include "ESPAsyncWebServer.h"
#include "AsyncWiFiManagerParameter.h"
#include "WifiResult.h"
#include "Logger.h"

using wifi_ssid_count_t = int16_t;

#define WIFI_MANAGER_MAX_PARAMS 10
static const long AUTO_CONNECT_RETRY_DELAY_MS = 1000;

class AsyncWiFiManager {
public:
    AsyncWiFiManager(AsyncWebServer *server, DNSServer *dns, Logger *logger);
    void scan(boolean async = false);
    auto scanModal() -> String;

    auto autoConnect(char const *apName,
                     char const *apPassword = nullptr,
                     unsigned long retryDelayMs = AUTO_CONNECT_RETRY_DELAY_MS) -> boolean;

    void resetSettings();

    // Sets a custom ip /gateway /subnet configuration
    void setAPStaticIPConfig(const IPAddress& ip, const IPAddress& gw, const IPAddress& sn);

    // Sets config for a static IP
    void setSTAStaticIPConfig(const IPAddress& ip,
                              const IPAddress& gw,
                              const IPAddress& sn,
                              const IPAddress& dns1 = (uint32_t)0x00000000,
                              const IPAddress& dns2 = (uint32_t)0x00000000);

    void addParameter(AsyncWiFiManagerParameter *p);

    // if this is set, it will exit after config, even if connection is unsuccessful
    void setBreakAfterConfig(boolean shouldBreak);
    auto startConfigPortal(char const *apName, char const *apPassword = nullptr) -> boolean;

private:
    AsyncWebServer *server;
    DNSServer *dnsServer;
    unsigned long scanNow{};
    boolean shouldScan = true;
    String pager;
    wl_status_t wifiStatus;
    const char *_apName = "no-net";
    const char *_apPassword = nullptr;
    String _ssid = "";
    String _pass = "";
    unsigned long _configPortalTimeout = 0;
    unsigned long _connectTimeout = 0;
    unsigned long _configPortalStart = 0;
    IPAddress _ap_static_ip;
    IPAddress _ap_static_gw;
    IPAddress _ap_static_sn;
    IPAddress _sta_static_ip;
    IPAddress _sta_static_gw;
    IPAddress _sta_static_sn;
    IPAddress _sta_static_dns1 = (uint32_t)0x00000000;
    IPAddress _sta_static_dns2 = (uint32_t)0x00000000;
    unsigned int _paramsCount = 0;
    unsigned int _minimumQuality = 0;
    boolean _shouldBreakAfterConfig = false;
    const char *_customHeadElement = "";
    auto connectWifi(const String& ssid, const String& pass) -> uint8_t;
    auto waitForConnectResult() -> uint8_t;
    void copySSIDInfo(wifi_ssid_count_t n);
    auto buildSsidListHtml() -> String;
    void setupConfigPortal();
    void handleConfigureWifi(AsyncWebServerRequest *request, boolean scan);
    void handleWifiSaveForm(AsyncWebServerRequest *request);
    void handleNotFound(AsyncWebServerRequest *);
    auto tryRedirectToCaptivePortal(AsyncWebServerRequest *request) -> boolean;
    static auto getRSSIasQuality(int RSSI) -> unsigned int;
    auto isIpAddress(const String& str) -> boolean;
    static auto convertIpAddressToString(const IPAddress& ip) -> String;
    boolean connect{};
    WiFiResult *wifiSSIDs;
    wifi_ssid_count_t wifiSSIDCount{};
    boolean wifiSsidScan;
    boolean _tryConnectDuringConfigPortal = true;
    std::function<void(AsyncWiFiManager *)> _apCallback;
    std::function<void()> _saveCallback;
    AsyncWiFiManagerParameter *_params[WIFI_MANAGER_MAX_PARAMS]{};
    Logger *logger;
    static auto accessPointFilter(AsyncWebServerRequest *request) -> bool;

    template <class T>
    auto optionalIPFromString(T *obj, const char *s) -> decltype(obj->fromString(s))
    {
        return obj->fromString(s);
    }


};

#endif //MODBUS_TO_MQTT_ASYNCWIFIMANAGER_H
