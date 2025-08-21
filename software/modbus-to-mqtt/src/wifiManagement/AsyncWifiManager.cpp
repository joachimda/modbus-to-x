/**************************************************************
    Originally part of WiFiManager for the ESP8266/Arduino platform
   (https://github.com/tzapu/WiFiManager)
   Built by AlexT https://github.com/tzapu
   Ported to Async Web Server by https://github.com/alanswx
   Licensed under MIT license

   Modified by joachimda (2025)
    Changes include:
    - simplification to accommodate custom use.
    - removal of multi-device compatibility
    - Replacing serial logger with custom logger
 **************************************************************/

#include <utility>
#include "wifiManagement/AsyncWiFiManager.h"
#include "Helpers.h"
#include "wifiManagement/PortalPageBuilder.h"
#include "constants/HttpResponseCodes.h"
#include "constants/HttpMediaTypes.h"

static const size_t min_password_len = 8;
static const size_t max_password_len = 63;
static const uint32_t ap_startup_delay_ms = 500;
static const uint32_t persistent_connect_delay_ms = 100;
static const auto conn_status_check_interval_ms = 100UL;


AsyncWiFiManager::AsyncWiFiManager(AsyncWebServer *server, DNSServer *dns, Logger *logger)
        : server(server), dnsServer(dns), logger(logger)
{
    wifiSSIDs = nullptr;
    wifiSsidScan = true;
    _modeless = false;
    shouldScan = true;
}

void AsyncWiFiManager::addParameter(AsyncWiFiManagerParameter *p)
{
    _params[_paramsCount] = p;
    _paramsCount++;
    logger->logInformation(("Adding parameter: " +
                            String(p->getID())).c_str());
}

void AsyncWiFiManager::setupConfigPortal()
{
    server->reset();
    _configPortalStart = millis();

    logger->logInformation(("Configuring access point: " + String(_apName)).c_str());
    if (_apPassword != nullptr)
    {
        if (strlen(_apPassword) < min_password_len || strlen(_apPassword) > max_password_len)
        {
            logger->logError("Invalid AP password length.");
            _apPassword = nullptr;
        }
    }

    if (_ap_static_ip != 0U)
    {
        logger->logInformation("Custom AP IP/GW/Subnet");
        WiFi.softAPConfig(_ap_static_ip, _ap_static_gw, _ap_static_sn);
    }

    WiFi.softAP(_apName, _apPassword);

    delay(ap_startup_delay_ms); // without delay I've seen the IP address blank

    logger->logInformation(("AP IP address: " +
                            convertIpAddressToString(WiFi.softAPIP())).c_str());

    // set up the DNS server redirecting all the domains to the apIP
    dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    if (!dnsServer->start(DNS_PORT, "*", WiFi.softAPIP()))
    {
        logger->logError("Could not start Captive DNS Server!");
    }

    /*
    server->on("/",
               std::bind(&AsyncWiFiManager::handleRoot, this, std::placeholders::_1))
            .setFilter(accessPointFilter);
    server->on("/wifi",
               std::bind(&AsyncWiFiManager::handleConfigureWifi, this, std::placeholders::_1, true))
            .setFilter(accessPointFilter);

    server->on("/r",
               std::bind(&AsyncWiFiManager::handleReset, this, std::placeholders::_1))
            .setFilter(accessPointFilter);

            */

    server->on("/wifisave",
               std::bind(&AsyncWiFiManager::handleWifiSaveForm, this, std::placeholders::_1))
            .setFilter(accessPointFilter);
    server->onNotFound(std::bind(&AsyncWiFiManager::handleNotFound, this, std::placeholders::_1));
    server->begin();
    logger->logInformation("HTTP server started");
}
auto AsyncWiFiManager::autoConnect(char const *apName,
                                   char const *apPassword,
                                   unsigned long retryDelayMs) -> boolean
{
    unsigned long maxConnectRetries = 1;
    WiFiClass::mode(WIFI_STA);
    for (unsigned long tryNumber = 0; tryNumber < maxConnectRetries; tryNumber++)
    {
        logger->logInformation(("AutoConnect Try No.: " + String(tryNumber)).c_str());

        if (connectWifi("", "") == WL_CONNECTED)
        {
            logger->logInformation(("IP Address: " +
                                    convertIpAddressToString(WiFi.localIP())).c_str());
            return true;
        }

        if (tryNumber + 1 < maxConnectRetries)
        {
            // we might connect during the delay
            unsigned long restDelayMs = retryDelayMs;
            while (restDelayMs != 0)
            {
                if (WiFiClass::status() == WL_CONNECTED)
                {
                    logger->logInformation(("IP Address (connected during delay): " +
                                            convertIpAddressToString(WiFi.localIP())).c_str());
                    return true;
                }
                unsigned long thisDelay = std::min(restDelayMs, conn_status_check_interval_ms);
                delay(thisDelay);
                restDelayMs -= thisDelay;
            }
        }
    }

    return startConfigPortal(apName, apPassword);
}

auto AsyncWiFiManager::buildSsidListHtml() -> String
{
    String networkListPager;
    for (int i = 0; i < wifiSSIDCount; i++)
    {
        if (wifiSSIDs[i].duplicate)
        {
            continue;
        }
        unsigned int quality = getRSSIasQuality(wifiSSIDs[i].RSSI);

        if (_minimumQuality == 0 || _minimumQuality < quality)
        {
            String item = FPSTR(HTTP_ITEM);
            String rssiQ;
            rssiQ += quality;
            item.replace("{v}", wifiSSIDs[i].SSID);
            item.replace("{r}", rssiQ);
            if (wifiSSIDs[i].encryptionType != WIFI_AUTH_OPEN)
            {
                item.replace("{i}", "l");
            }
            else
            {
                item.replace("{i}", "");
            }
            networkListPager += item;
        }
        else
        {
            logger->logWarning("Skipping due to quality");
        }
    }
    return networkListPager;
}

auto AsyncWiFiManager::scanModal() -> String
{
    shouldScan = true;
    scan();
    String networkListPager = buildSsidListHtml();
    return networkListPager;
}

void AsyncWiFiManager::scan(boolean async)
{
    if (!shouldScan)
    {
        return;
    }
    logger->logInformation("About to scan");
    if (wifiSsidScan)
    {
        wifi_ssid_count_t n = WiFi.scanNetworks(async);
        copySSIDInfo(n);
    }
}

void AsyncWiFiManager::copySSIDInfo(wifi_ssid_count_t n)
{
    if (n == WIFI_SCAN_FAILED)
    {
        logger->logWarning("scanNetworks returned: WIFI_SCAN_FAILED!");
    }
    else if (n == WIFI_SCAN_RUNNING)
    {
        logger->logInformation("scanNetworks returned: WIFI_SCAN_RUNNING!");
    }
    else if (n < 0)
    {
        logger->logError(("scanNetworks failed with unknown error code: " + String(n)).c_str());
    }
    else if (n == 0)
    {
        logger->logInformation("No networks found");
    }
    else
    {
        logger->logInformation("Scan done");
    }

    if (n > 0)
    {
        // WE SHOULD MOVE THIS IN PLACE ATOMICALLY
        if (wifiSSIDs)
        {
            delete[] wifiSSIDs;
        }
        wifiSSIDs = new WiFiResult[n];
        wifiSSIDCount = n;

        shouldScan = false;
        for (wifi_ssid_count_t i = 0; i < n; i++)
        {
            wifiSSIDs[i].duplicate = false;


            WiFi.getNetworkInfo(i,
                                wifiSSIDs[i].SSID,
                                wifiSSIDs[i].encryptionType,
                                wifiSSIDs[i].RSSI,
                                wifiSSIDs[i].BSSID,
                                wifiSSIDs[i].channel);
        }

        // RSSI SORT
        for (int i = 0; i < n; i++)
        {
            for (int j = i + 1; j < n; j++)
            {
                if (wifiSSIDs[j].RSSI > wifiSSIDs[i].RSSI)
                {
                    std::swap(wifiSSIDs[i], wifiSSIDs[j]);
                }
            }
        }

        // remove duplicates ( must be RSSI sorted )
        String c_ssid;
        for (int i = 0; i < n; i++)
        {
            if (wifiSSIDs[i].duplicate)
            {
                continue;
            }
            c_ssid = wifiSSIDs[i].SSID;
            for (int j = i + 1; j < n; j++)
            {
                if (c_ssid == wifiSSIDs[j].SSID)
                {
                    logger->logInformation(("DUP AP: " + wifiSSIDs[j].SSID).c_str());
                    wifiSSIDs[j].duplicate = true; // set dup aps to NULL
                }
            }
        }
    }
}

auto AsyncWiFiManager::startConfigPortal(char const *apName, char const *apPassword) -> boolean
{
    WiFiClass::mode(WIFI_AP_STA);
    logger->logInformation("SET AP STA");

    _apName = apName;
    _apPassword = apPassword;
    bool connectedDuringConfigPortal = false;

    if (_apCallback != nullptr)
    {
        _apCallback(this);
    }

    connect = false;
    setupConfigPortal();
    scanNow = 0;
    while (_configPortalTimeout == 0 || millis() - _configPortalStart < _configPortalTimeout)
    {
        dnsServer->processNextRequest();
        if (scanNow == 0 || millis() - scanNow >= 10000)
        {
            logger->logInformation("About to scan");
            shouldScan = true; // since we are modal, we can scan every time

            WiFi.disconnect(false);
            scanModal();
            if (_tryConnectDuringConfigPortal)
            {
                WiFi.begin(); // try to reconnect to AP
                connectedDuringConfigPortal = true;
            }
            scanNow = millis();
        }

        // attempts to reconnect were successful
        if (WiFiClass::status() == WL_CONNECTED)
        {
            // connected
            WiFiClass::mode(WIFI_STA);
            // notify that configuration has changed and any optional parameters should be saved
            // configuration should not be saved when just connected using stored ssid and password during config portal
            if (!connectedDuringConfigPortal && _saveCallback != nullptr)
            {
                // TODO: check if any custom parameters actually exist, and check if they really changed maybe
                _saveCallback();
            }
            break;
        }

        if (connect)
        {
            connect = false;
            delay(2000);
            logger->logInformation("Connecting to new AP");

            // using user-provided _ssid, _pass in place of system-stored ssid and pass
            WiFi.persistent(true);
            if (_tryConnectDuringConfigPortal and connectWifi(_ssid, _pass) == WL_CONNECTED)
            {
                WiFi.persistent(false);
                // connected
                WiFiClass::mode(WIFI_STA);
                // notify that configuration has changed and any optional parameters should be saved
                if (_saveCallback != nullptr)
                {
                    _saveCallback();
                }
                break;
            }

            if(_tryConnectDuringConfigPortal) {
                logger->logError("Failed to connect");
            }

            if (_shouldBreakAfterConfig)
            {
                // flag set to exit after config after trying to connect
                // notify that configuration has changed and any optional parameters should be saved
                if (_saveCallback != nullptr)
                {
                    _saveCallback();
                }
                break;
            }
        }
        yield();
    }

    server->reset();
    dnsServer->stop();

    return WiFiClass::status() == WL_CONNECTED;
}

auto AsyncWiFiManager::connectWifi(const String& ssid, const String& pass) -> uint8_t
{
    logger->logInformation("Connecting as wifi client...");

    // check if we've got static_ip settings, if we do, use those
    if (_sta_static_ip != 0U)
    {
        WiFi.config(_sta_static_ip, _sta_static_gw, _sta_static_sn, _sta_static_dns1, _sta_static_dns2);
        logger->logInformation(("Custom STA IP/GW/Subnet/DNS: " +
                                convertIpAddressToString(WiFi.localIP())).c_str());
    }

    // check if we have ssid and pass and force those, if not, try with last saved values
    if (ssid != "")
    {
        WiFi.disconnect(false);
        WiFi.begin(ssid.c_str(), pass.c_str());
    }
    else
    {
        if (WiFi.SSID().length() > 0)
        {
            logger->logInformation("Using last saved values, should be faster");

            WiFi.disconnect(false);
            WiFi.begin();
        }
        else
        {
            logger->logInformation("Try to connect with saved credentials");
            WiFi.begin();
        }
    }

    uint8_t connRes = waitForConnectResult();
    logger->logInformation(("Connection result: " + String(connRes)).c_str());
    return connRes;
}

auto AsyncWiFiManager::waitForConnectResult() -> uint8_t
{
    if (_connectTimeout == 0)
    {
        return WiFi.waitForConnectResult();
    }

    logger->logInformation("Waiting for connection result with time out");
    unsigned long start = millis();
    boolean keepConnecting = true;
    uint8_t WifiSTA_Status;
    while (keepConnecting)
    {
        WifiSTA_Status = WiFiClass::status();
        if (millis() > start + _connectTimeout)
        {
            keepConnecting = false;
            logger->logWarning("Connection timed out");
        }
        if (WifiSTA_Status == WL_CONNECTED || WifiSTA_Status == WL_CONNECT_FAILED)
        {
            keepConnecting = false;
        }
        delay(persistent_connect_delay_ms);
    }
    return WifiSTA_Status;
}

void AsyncWiFiManager::resetSettings()
{
    logger->logInformation("Settings invalidated");
    logger->logInformation("THIS MAY CAUSE AP NOT TO START UP PROPERLY. YOU NEED TO COMMENT IT OUT AFTER ERASING THE DATA.");

    WiFiClass::mode(WIFI_AP_STA);
    WiFi.persistent(true);
    WiFi.disconnect(true, true);
    WiFi.persistent(false);
}

void AsyncWiFiManager::setAPStaticIPConfig(const IPAddress& ip,
                                           const IPAddress& gw,
                                           const IPAddress& sn)
{
    _ap_static_ip = ip;
    _ap_static_gw = gw;
    _ap_static_sn = sn;
}

void AsyncWiFiManager::setSTAStaticIPConfig(const IPAddress& ip,
                                            const IPAddress& gw,
                                            const IPAddress& sn,
                                            const IPAddress& dns1,
                                            const IPAddress& dns2)
{
    _sta_static_ip = ip;
    _sta_static_gw = gw;
    _sta_static_sn = sn;
    _sta_static_dns1 = dns1;
    _sta_static_dns2 = dns2;
}

void AsyncWiFiManager::setBreakAfterConfig(boolean shouldBreak)
{
    _shouldBreakAfterConfig = shouldBreak;
}

void AsyncWiFiManager::handleRoot(AsyncWebServerRequest *request)
{
    logger->logDebug("Handle root");

    shouldScan = true;
    scanNow = 0;

    if (tryRedirectToCaptivePortal(request))
    {
        // if captive portal redirect instead of displaying the page
        return;
    }
    String page = PortalPageBuilder::buildRoot(_apName);

    request->send(HttpResponseCodes::OK, HttpMediaTypes::HTML, page);
}

void AsyncWiFiManager::handleConfigureWifi(AsyncWebServerRequest *request, boolean scan)
{
    shouldScan = true;
    scanNow = 0;

    logger->logInformation("Handle wifi");

    String page = FPSTR(WFM_HTTP_HEAD);
    page.replace("{v}", "Config ESP");
    page += FPSTR(HTTP_SCRIPT);
    page += FPSTR(HTTP_STYLE);
    page += _customHeadElement;
    page += FPSTR(HTTP_HEAD_END);

    if (scan)
    {
        wifiSsidScan = false;

        logger->logInformation("Scan done");
        if (wifiSSIDCount == 0)
        {
            logger->logInformation("No networks found");
            page += F("No networks found. Refresh to scan again");
        }
        else
        {
            // display networks in page
            String networkListPager = buildSsidListHtml();
            page += networkListPager;
            page += "<br/>";
        }
    }
    wifiSsidScan = true;

    page += FPSTR(HTTP_FORM_START);
    char parLength[2];

    // add the extra parameters to the form
    for (unsigned int i = 0; i < _paramsCount; i++)
    {
        if (_params[i] == nullptr)
        {
            break;
        }

        String p_item = FPSTR(HTTP_FORM_PARAM);
        if (_params[i]->getID() != nullptr)
        {
            p_item.replace("{i}", _params[i]->getID());
            p_item.replace("{n}", _params[i]->getID());
            p_item.replace("{p}", _params[i]->getPlaceholder());
            snprintf(parLength, 2, "%d", _params[i]->getValueLength());
            p_item.replace("{l}", parLength);
            p_item.replace("{v}", _params[i]->getValue());
            p_item.replace("{c}", _params[i]->getCustomHTML());
        }
        else
        {
            p_item = _params[i]->getCustomHTML();
        }

        page += p_item;
    }
    if (_params[0] != nullptr)
    {
        page += "<br/>";
    }
    if (_sta_static_ip != 0U)
    {
        String item = FPSTR(HTTP_FORM_PARAM);
        item.replace("{i}", "ip");
        item.replace("{n}", "ip");
        item.replace("{p}", "Static IP");
        item.replace("{l}", "15");
        item.replace("{v}", _sta_static_ip.toString());

        page += item;

        item = FPSTR(HTTP_FORM_PARAM);
        item.replace("{i}", "gw");
        item.replace("{n}", "gw");
        item.replace("{p}", "Static Gateway");
        item.replace("{l}", "15");
        item.replace("{v}", _sta_static_gw.toString());

        page += item;

        item = FPSTR(HTTP_FORM_PARAM);
        item.replace("{i}", "sn");
        item.replace("{n}", "sn");
        item.replace("{p}", "Subnet");
        item.replace("{l}", "15");
        item.replace("{v}", _sta_static_sn.toString());

        page += item;

        item = FPSTR(HTTP_FORM_PARAM);
        item.replace("{i}", "dns1");
        item.replace("{n}", "dns1");
        item.replace("{p}", "DNS1");
        item.replace("{l}", "15");
        item.replace("{v}", _sta_static_dns1.toString());

        page += item;

        item = FPSTR(HTTP_FORM_PARAM);
        item.replace("{i}", "dns2");
        item.replace("{n}", "dns2");
        item.replace("{p}", "DNS2");
        item.replace("{l}", "15");
        item.replace("{v}", _sta_static_dns2.toString());

        page += item;
        page += "<br/>";
    }
    page += FPSTR(HTTP_FORM_END);
    page += FPSTR(HTTP_SCAN_LINK);
    page += FPSTR(HTTP_END);

    request->send(HttpResponseCodes::OK, HttpMediaTypes::HTML, page);

    logger->logDebug("Sent config page");
}

// handle the WLAN save form and redirect to WLAN config page again
void AsyncWiFiManager::handleWifiSaveForm(AsyncWebServerRequest *request)
{
    logger->logInformation("WiFi save");

    _ssid = request->arg("s").c_str();
    _pass = request->arg("p").c_str();

    // parameters
    for (unsigned int i = 0; i < _paramsCount; i++)
    {
        if (_params[i] == nullptr)
        {
            break;
        }
        String value = request->arg(_params[i]->getID()).c_str();
        value.toCharArray(_params[i]->_value, _params[i]->_length);
        logger->logInformation(("Parameter " + value).c_str());
    }

    if (request->hasArg("ip"))
    {
        const String& ip = request->arg("ip");
        logger->logInformation(("static ip: " + ip).c_str());
        optionalIPFromString(&_sta_static_ip, ip.c_str());
    }
    if (request->hasArg("gw"))
    {
        const String& gw = request->arg("gw");
        logger->logInformation(("static gateway: " + gw).c_str());
        optionalIPFromString(&_sta_static_gw, gw.c_str());
    }
    if (request->hasArg("sn"))
    {
        const String& sn = request->arg("sn");
        logger->logInformation(("static netmask: " + sn).c_str());
        optionalIPFromString(&_sta_static_sn, sn.c_str());
    }
    if (request->hasArg("dns1"))
    {
        const String& dns1 = request->arg("dns1");
        logger->logInformation(("static DNS 1: " + dns1).c_str());
        optionalIPFromString(&_sta_static_dns1, dns1.c_str());
    }
    if (request->hasArg("dns2"))
    {
        const String& dns2 = request->arg("dns2");

        logger->logInformation(("static DNS 2: " + dns2).c_str());
        optionalIPFromString(&_sta_static_dns2, dns2.c_str());
    }

    String page = FPSTR(WFM_HTTP_HEAD);
    page.replace("{v}", "Credentials Saved");
    page += FPSTR(HTTP_SCRIPT);
    page += FPSTR(HTTP_STYLE);
    page += _customHeadElement;
    page += F("<meta http-equiv=\"refresh\" content=\"5; url=/i\">");
    page += FPSTR(HTTP_HEAD_END);
    page += FPSTR(HTTP_SAVED);
    page += FPSTR(HTTP_END);

    request->send(HttpResponseCodes::OK,HttpMediaTypes::HTML, page);

    logger->logDebug("Sent wifi save page");

    connect = true;
}

void AsyncWiFiManager::handleReset(AsyncWebServerRequest *request)
{
    logger->logInformation("Reset");

    String page = FPSTR(WFM_HTTP_HEAD);
    page.replace("{v}", "Info");
    page += FPSTR(HTTP_SCRIPT);
    page += FPSTR(HTTP_STYLE);
    page += _customHeadElement;
    page += FPSTR(HTTP_HEAD_END);
    page += F("Module will reset in a few seconds");
    page += FPSTR(HTTP_END);
    request->send(HttpResponseCodes::OK, HttpMediaTypes::HTML, page);

    logger->logInformation("Sent reset page");
    delay(5000);
    ESP.restart();
    delay(2000);
}

void AsyncWiFiManager::handleNotFound(AsyncWebServerRequest *request)
{
    logger->logError("Handle not found");

    if (tryRedirectToCaptivePortal(request))
    {
        // if captive portal redirect instead of displaying the error page
        return;
    }

    String message = "File Not Found\n\n";
    message += "URI: ";
    message += request->url();
    message += "\nMethod: ";
    message += (request->method() == HTTP_GET) ? "GET" : "POST";
    message += "\nArguments: ";
    message += request->args();
    message += "\n";

    for (unsigned int i = 0; i < request->args(); i++)
    {
        message += " " + request->argName(i) + ": " + request->arg(i) + "\n";
    }
    AsyncWebServerResponse *response = request->beginResponse(HttpResponseCodes::NOT_FOUND, HttpMediaTypes::PLAIN_TEXT, message);
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "-1");
    request->send(response);
}

/** Redirect to captive portal if we got a request for another domain.
 * Return true in that case so the page handler do not try to handle the request again. */
boolean AsyncWiFiManager::tryRedirectToCaptivePortal(AsyncWebServerRequest *request)
{
    logger->logInformation(("tryRedirectToCaptivePortal - Request.Host: " +
                            request->host()).c_str());

    if (!isIpAddress(request->host()))
    {
        logger->logInformation("Request redirected to captive portal");
        AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
        response->addHeader("Location", String("http://") + convertIpAddressToString(request->client()->localIP()));
        request->send(response);
        return true;
    }
    return false;
}

unsigned int AsyncWiFiManager::getRSSIasQuality(int RSSI)
{
    unsigned int quality = 0;

    if (RSSI <= -100)
    {
        quality = 0;
    }
    else if (RSSI >= -50)
    {
        quality = 100;
    }
    else
    {
        quality = 2 * (RSSI + 100);
    }
    return quality;
}

auto AsyncWiFiManager::isIpAddress(const String& str) -> boolean
{
    logger->logDebug("isIpAddress - begin");
    for (unsigned int i = 0; i < str.length(); i++)
    {
        int c = str.charAt(i);
        if (c != '.' && (c < '0' || c > '9'))
        {
            logger->logDebug(("isIpAddress - " + str + " is not an IP").c_str());
            return false;
        }
    }
    logger->logDebug(("isIpAddress - " + str + " is an IP").c_str());
    logger->logDebug("isIpAddress - end");
    return true;
}

auto AsyncWiFiManager::convertIpAddressToString(const IPAddress& ip) -> String
{
    String res = "";
    for (int i = 0; i < 3; i++)
    {
        res += String((ip >> (8 * i)) & 0xFF) + ".";
    }
    res += String(((ip >> 8 * 3)) & 0xFF);
    return res;
}

auto AsyncWiFiManager::accessPointFilter(AsyncWebServerRequest *request) -> bool {

    return WiFi.localIP() != request->client()->localIP();
}