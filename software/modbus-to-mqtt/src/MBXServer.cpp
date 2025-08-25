#include "MBXServer.h"
#include "Logger.h"
#include "constants/HttpResponseCodes.h"
#include "constants/HttpMediaTypes.h"
#include "constants/Routes.h"
#include "wifiManagement/AsyncWiFiManager.h"
#include "Config.h"
#include <ESPAsyncWebServer.h>
#include <cstdint>
#include <WiFi.h>

static constexpr auto WIFI_CONNECT_DELAY_MS = 100;
static constexpr auto WIFI_CONNECT_TIMEOUT = 10000;

MBXServer::MBXServer(AsyncWebServer * server, Logger * logger) : server(server), _logger(logger) {}

void MBXServer::networkBootstrap() {
    AsyncWiFiManager wm(server, dns, _logger);
    const unsigned long firstAttempt = millis();
    while (!wm.autoConnect(DEFAULT_AP_SSID, DEFAULT_AP_PASS)) {
        if (millis() - firstAttempt > WIFI_CONNECT_TIMEOUT) {
            ESP.restart();
        }
        delay(WIFI_CONNECT_DELAY_MS);
    }
}
void MBXServer::ensureConfigFile() {
    if (!SPIFFS.begin(true)) {
        _logger->logError("MBXServer::ensureConfigFile - File System error");
        return;
    }

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

void MBXServer::handleNetworkReset() {
    WiFiClass::mode(WIFI_AP_STA);
    WiFi.persistent(true);
    WiFi.disconnect(true, true);
    WiFi.persistent(false);
}

void MBXServer::handleUpload(AsyncWebServerRequest *r, const String& fn, size_t index, uint8_t *data, size_t len, bool final) {

    static File uploadFile;
    if (index == 0U) {
        uploadFile = SPIFFS.open("/config.json", FILE_WRITE);
    }
    if (uploadFile) {
        uploadFile.write(data, len);
    }
    if (final && uploadFile) {
        uploadFile.close();
    }
}

auto safeWriteFile(fs::FS& fs, const char* path, const String& content) -> bool {
    String tmp = String(path) + ".tmp";
    File f = fs.open(tmp, FILE_WRITE);
    if (!f) {
        return false;
    }
    size_t n = f.print(content);

    f.flush(); f.close();
    if (n != content.length()) {
        fs.remove(tmp);
        return false;
    }
    fs.remove(path);
    return fs.rename(tmp, path);
}

auto MBXServer::readConfig() -> String {
    if (!SPIFFS.exists("/config.json")) {
        _logger->logError("MBXServer::readConfig - File System error");
        return "{}";
    }

    File file = SPIFFS.open("/config.json", FILE_READ);
    String json = file.readString();
    file.close();
    return json;
}

void MBXServer::configurePageRoutes() {

    _logger->logDebug("MBXServer::configurePageRoutes - begin");
    server->serveStatic("/", SPIFFS, "/")
            .setDefaultFile("/index.html")
            .setCacheControl("no-store");

    server->on(Routes::ROOT, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(SPIFFS, "/index.html", HttpMediaTypes::HTML);
    });

    server->on(Routes::CONFIGURE, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(SPIFFS, "/pages/configure.html", HttpMediaTypes::HTML);
    });

    server->on(Routes::UPLOAD, WebRequestMethod::HTTP_POST, [](AsyncWebServerRequest *req) {
        req->send(HttpResponseCodes::OK, HttpMediaTypes::PLAIN_TEXT, "Upload OK");
    }, handleUpload);

    server->on(Routes::DOWNLOAD_CFG, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *req) {
        if (SPIFFS.exists("/conf/config.json")) {
            req->send(SPIFFS, "/conf/config.json", HttpMediaTypes::JSON);
        } else {
            req->send(HttpResponseCodes::NOT_FOUND, HttpMediaTypes::PLAIN_TEXT,"config.json not found!");
        }
    });

    server->on(Routes::DOWNLOAD_CFG_EX, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *req) {
        if(!SPIFFS.begin(true)) {
            req->send(HttpResponseCodes::SERVER_ERROR, HttpMediaTypes::PLAIN_TEXT,"System Error");
            return;
        }
        if (SPIFFS.exists("/conf/example.json")) {
            req->send(SPIFFS, "/conf/example.json", HttpMediaTypes::JSON);
        } else {
            req->send(HttpResponseCodes::NOT_FOUND, HttpMediaTypes::PLAIN_TEXT,"No config file found!");
        }
    });

    server->on(Routes::RESET_NETWORK, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *req) {
        if(!SPIFFS.begin(true)) {
            req->send(HttpResponseCodes::SERVER_ERROR, HttpMediaTypes::PLAIN_TEXT, "System Error");
            return;
        }
        if (SPIFFS.exists("/pages/reset_result.html")) {
            req->send(SPIFFS, "/pages/reset_result.html", HttpMediaTypes::HTML);
            handleNetworkReset();
        } else {
            req->send(HttpResponseCodes::NOT_FOUND, HttpMediaTypes::PLAIN_TEXT,"Page not found");
        }
    }).setFilter(accessPointFilter);

    _logger->logDebug("MBXServer::configurePageRoutes - end");
}

void MBXServer::configureApiRoutes() {
    _logger->logDebug("MBXServer::configureApiRoutes - begin");
    _logger->logDebug("MBXServer::configureApiRoutes - end");
}

void MBXServer::begin() {

    _logger->logDebug("MBXServer::begin - begin");
    if (!SPIFFS.begin(true)) {
        _logger->logError("An error occurred while mounting SPIFFS");
        return;
    }

    ensureConfigFile();
    configurePageRoutes();
    networkBootstrap();
    server->begin();
    _logger->logInformation("Web server started successfully");
    _logger->logDebug("MBXServer::begin - end");
}
auto MBXServer::accessPointFilter(AsyncWebServerRequest *request) -> bool {

    return WiFi.localIP() != request->client()->localIP();
}