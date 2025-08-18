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

typedef int16_t wifi_ssid_count_t;

#define WIFI_MANAGER_MAX_PARAMS 10

class AsyncWiFiManager
{
public:
    AsyncWiFiManager(AsyncWebServer *server, DNSServer *dns, Logger *logger);
    void scan(boolean async = false);
    auto scanModal() -> String;
    void loop();
    void safeLoop();
    void criticalLoop();
    static auto buildInfoHtml() -> String;

    auto autoConnect(char const *apName,
                        char const *apPassword = nullptr,
                        unsigned long maxConnectRetries = 1,
                        unsigned long retryDelayMs = 1000) -> boolean;

    // if you want to always start the config portal, without trying to connect first
    auto startConfigPortal(char const *apName, char const *apPassword = nullptr) -> boolean;
    void startConfigPortalModeless(char const *apName, char const *apPassword);

    // get the AP name of the config portal, so it can be used in the callback
    auto getConfigPortalSSID() -> String;

    void resetSettings();

    // defaults to not showing anything under 8% signal quality if called
    void setMinimumSignalQuality(unsigned int quality = 8);
    // sets a custom ip /gateway /subnet configuration
    void setAPStaticIPConfig(const IPAddress& ip, const IPAddress& gw, const IPAddress& sn);
    // sets config for a static IP
    void setSTAStaticIPConfig(const IPAddress& ip,
                              const IPAddress& gw,
                              const IPAddress& sn,
                              const IPAddress& dns1 = (uint32_t)0x00000000,
                              const IPAddress& dns2 = (uint32_t)0x00000000);
    // called when AP mode and config portal is started
    void setAPCallback(std::function<void(AsyncWiFiManager *)>);
    // called when settings have been changed and connection was successful
    void setSaveConfigCallback(std::function<void()> func);
    void addParameter(AsyncWiFiManagerParameter *p);
    // if this is set, it will exit after config, even if connection is unsuccessful
    void setBreakAfterConfig(boolean shouldBreak);

    // if this is true, remove duplicated Access Points - default true
    void setRemoveDuplicateAPs(boolean removeDuplicates);

    auto getConfiguredSTASSID() -> String{
        return _ssid;
    }
    auto getConfiguredSTAPassword() -> String{
        return _pass;
    }

private:
    AsyncWebServer *server;
    DNSServer *dnsServer;
    boolean _modeless;
    unsigned long scanNow{};
    boolean shouldScan = true;
    boolean needInfo = true;
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
    boolean _removeDuplicateAPs = true;
    boolean _shouldBreakAfterConfig = false;

    const char *_customHeadElement = "";

    uint8_t status = WL_IDLE_STATUS;
    auto connectWifi(const String& ssid, const String& pass) -> uint8_t;
    auto waitForConnectResult() -> uint8_t;
    void setInfo();
    void copySSIDInfo(wifi_ssid_count_t n);
    auto buildSsidListHtml() -> String;
    void setupConfigPortal();
    void handleRoot(AsyncWebServerRequest *);
    void handleConfigureWifi(AsyncWebServerRequest *request, boolean scan);
    void handleWifiSaveForm(AsyncWebServerRequest *request);
    void handleBuildInfoHtml(AsyncWebServerRequest *);
    void handleReset(AsyncWebServerRequest *);
    void handleNotFound(AsyncWebServerRequest *);
    auto tryRedirectToCaptivePortal(AsyncWebServerRequest *request) -> boolean;
    const byte DNS_PORT = 53;
    static auto getRSSIasQuality(int RSSI) -> unsigned int;
    auto isIpAddress(const String& str) -> boolean;
    static auto convertIpAddressToString(const IPAddress& ip) -> String;

    boolean connect{};
    boolean _debug = true;

    WiFiResult *wifiSSIDs;
    wifi_ssid_count_t wifiSSIDCount{};
    boolean wifiSsidScan;

    boolean _tryConnectDuringConfigPortal = true;

    std::function<void(AsyncWiFiManager *)> _apCallback;
    std::function<void()> _saveCallback;

    AsyncWiFiManagerParameter *_params[WIFI_MANAGER_MAX_PARAMS]{};

    template <class T>
    auto optionalIPFromString(T *obj, const char *s) -> decltype(obj->fromString(s))
    {
        return obj->fromString(s);
    }

    Logger *logger;

   static bool accessPointFilter(AsyncWebServerRequest *request);
};

#endif //MODBUS_TO_MQTT_ASYNCWIFIMANAGER_H
