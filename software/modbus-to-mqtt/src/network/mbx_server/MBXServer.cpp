#include "network/mbx_server/MBXServer.h"
#include "Logger.h"
#include "constants/HttpResponseCodes.h"
#include "constants/HttpMediaTypes.h"
#include "constants/Routes.h"
#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include "network/NetworkPortal.h"
#include "network/mbx_server/MBXServerHandlers.h"

static constexpr auto WIFI_CONNECT_DELAY_MS = 100;
static constexpr auto WIFI_CONNECT_TIMEOUT = 10000;
static WiFiConnectController g_wifi; // global/singleton

MBXServer::MBXServer(AsyncWebServer *server, DNSServer *dnsServer, Logger *logger) : server(server), _dns(dnsServer),
    _logger(logger) {
}

void MBXServer::begin() const {
    _logger->logDebug("MBXServer::begin - begin");
    if (!SPIFFS.begin(true)) {
        _logger->logError("An error occurred while mounting SPIFFS");
        return;
    }
    _logger->logDebug(("MBXServer::begin - SSID Stored in NVS: " + String(WiFi.SSID())).c_str());

    ensureConfigFile();
    if (tryConnectWithStoredCreds()) {
        configurePageRoutes();
        server->begin();
    } else {

        g_wifi.begin("modbus-to-x");

        NetworkPortal portal(_logger, _dns);
        MBXServerHandlers::setPortal(&portal);
        configureAccessPointRoutes();
        server->begin();
        portal.begin();
    }

    _logger->logDebug("MBXServer::begin - end");
}

void MBXServer::loop() {
    g_wifi.loop();
}

void MBXServer::configureAccessPointRoutes() const {
    _logger->logDebug("MBXServer::configureAccessPointRoutes - begin");

    server->serveStatic("/", SPIFFS, "/")
            .setDefaultFile("/pages/configure_network.html")
            .setCacheControl("no-store");

    server->on(Routes::ROOT, HTTP_GET, [](AsyncWebServerRequest *req) {
        serveSPIFFSFile(req, "/pages/configure_network.html", nullptr, HttpMediaTypes::HTML);
    });

    server->on(Routes::GET_SSID_LIST, HTTP_GET, [](AsyncWebServerRequest *req) {
        MBXServerHandlers::getSsidListAsJson(req);
    });

    // POST with JSON body
    server->on("/api/wifi/connect", HTTP_POST,
               [](AsyncWebServerRequest *req) {
               },
               nullptr,
               [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
                   MBXServerHandlers::handleWifiConnect(req, g_wifi, data, len, index, total);
               }
    );
    server->on(Routes::RESET_NETWORK, HTTP_GET, [](AsyncWebServerRequest *req) {
        serveSPIFFSFile(req, "/pages/reset_result.html", MBXServerHandlers::handleNetworkReset, HttpMediaTypes::HTML);
    }).setFilter(accessPointFilter);
    server->on("/api/wifi/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        MBXServerHandlers::handleWifiStatus(req, g_wifi);
    });
    server->on("/api/wifi/ap_off", HTTP_POST, MBXServerHandlers::handleWifiApOff);
    server->on("/api/wifi/cancel", HTTP_POST, [](AsyncWebServerRequest *req) {
        MBXServerHandlers::handleWifiCancel(req, g_wifi);
    });
    _logger->logDebug("MBXServer::configureAccessPointRoutes - end");
}

void MBXServer::configurePageRoutes() const {
    _logger->logDebug("MBXServer::configurePageRoutes - begin");
    server->serveStatic("/", SPIFFS, "/")
            .setDefaultFile("/index.html")
            .setCacheControl("no-store");

    server->on(Routes::ROOT, HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(SPIFFS, "/index.html", HttpMediaTypes::HTML);
    });

    server->on(Routes::CONFIGURE, HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(SPIFFS, "/pages/configure.html", HttpMediaTypes::HTML);
    });

    server->on(Routes::UPLOAD, HTTP_POST, [](AsyncWebServerRequest *req) {
        req->send(HttpResponseCodes::OK, HttpMediaTypes::PLAIN_TEXT, "Upload OK");
    }, MBXServerHandlers::handleUpload);;

    server->on(Routes::DOWNLOAD_CFG, HTTP_GET, [](AsyncWebServerRequest *req) {
        serveSPIFFSFile(req, "/conf/config.json", nullptr, HttpMediaTypes::JSON);
    });

    server->on(Routes::DOWNLOAD_CFG_EX, HTTP_GET, [](AsyncWebServerRequest *req) {
        serveSPIFFSFile(req, "/conf/example.json", nullptr, HttpMediaTypes::JSON);
    });

    server->on(Routes::RESET_NETWORK, HTTP_GET, [](AsyncWebServerRequest *req) {
        serveSPIFFSFile(req, "/pages/reset_result.html", MBXServerHandlers::handleNetworkReset, HttpMediaTypes::HTML);
    }).setFilter(accessPointFilter);

    _logger->logDebug("MBXServer::configurePageRoutes - end");
}

auto MBXServer::accessPointFilter(AsyncWebServerRequest *request) -> bool {
    // True when the HTTP connection is to this AP interface,
    const IPAddress dest = request->client()->localIP();
    return dest == WiFi.softAPIP();
}

auto MBXServer::tryConnectWithStoredCreds() const -> bool {
    _logger->logDebug("MBXServer::tryConnectWithStoredCreds - begin");
    _logger->logDebug(("MBXServer::tryConnectWithStoredCreds - #1 SSID Stored in NVS: " + String(WiFi.SSID())).c_str());

    WiFi.persistent(false);
    // Ensure STA mode for this attempt
    if (WiFiClass::getMode() != WIFI_MODE_STA) {
        WiFiClass::mode(WIFI_MODE_STA);
        delay(10);
    }
    _logger->logDebug(("MBXServer::tryConnectWithStoredCreds - #2 SSID Stored in NVS: " + String(WiFi.SSID())).c_str());
    WiFi.begin();

    const unsigned long start = millis();
    while (millis() - start < WIFI_CONNECT_TIMEOUT) {
        if (WiFiClass::status() == WL_CONNECTED) {
            _logger->logInformation(("Connected to WiFi: " + WiFi.SSID() +
                                     " " + WiFi.localIP().toString()).c_str());
            return true;
        }
        delay(WIFI_CONNECT_DELAY_MS);
    }
    _logger->logInformation("MBXServer::tryConnectWithStoredCreds() - No connection with stored credentials; starting AP portal");
    return false;
}

void MBXServer::serveSPIFFSFile(AsyncWebServerRequest *reqPtr, const char *path, std::function<void()> onServed,
                                const char *contentType) {
    if (SPIFFS.exists(path)) {
        Serial.println("Serving file: " + String(path));
        reqPtr->send(SPIFFS, path, contentType);
        if (onServed) {
            Serial.println("Calling onServed");
            onServed();
            Serial.println("onServed called");
        }
    } else {
        Serial.println("File not found: " + String(path));
        reqPtr->send(HttpResponseCodes::NOT_FOUND, HttpMediaTypes::PLAIN_TEXT, "Page not found");
    }
}

void MBXServer::ensureConfigFile() const {
    if (!SPIFFS.exists("/conf/config.json")) {
        _logger->logWarning("MBXServer::ensureConfigFile - Config file not found. Creating new one");
        File file = SPIFFS.open("/conf/config.json", FILE_WRITE);
        if (!file) {
            return;
        }

        file.print("{}");
        file.close();
    }
}

auto MBXServer::safeWriteFile(FS &fs, const char *path, const String &content) -> bool {
    const String tmp = String(path) + ".tmp";
    File f = fs.open(tmp, FILE_WRITE);
    if (!f) {
        return false;
    }
    const size_t n = f.print(content);

    f.flush();
    f.close();
    if (n != content.length()) {
        fs.remove(tmp);
        return false;
    }
    fs.remove(path);
    return fs.rename(tmp, path);
}

auto MBXServer::readConfig() const -> String {
    if (!SPIFFS.exists("/config.json")) {
        _logger->logError("MBXServer::readConfig - File System error");
        return "{}";
    }

    File file = SPIFFS.open("/config.json", FILE_READ);
    String json = file.readString();
    file.close();
    return json;
}
