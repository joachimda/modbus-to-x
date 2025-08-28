#include "MBXServer.h"
#include "Logger.h"
#include "constants/HttpResponseCodes.h"
#include "constants/HttpMediaTypes.h"
#include "constants/Routes.h"
#include "wifiManagement/AsyncWiFiManager.h"
#include "MBXServerHandlers.h"
#include "wifiManagement/NetworkPortal.h"
#include <ESPAsyncWebServer.h>

static constexpr auto WIFI_CONNECT_DELAY_MS = 100;
static constexpr auto WIFI_CONNECT_TIMEOUT = 10000;

MBXServer::MBXServer(AsyncWebServer * server, DNSServer * dnsServer, Logger * logger) : server(server), _dns(dnsServer), _logger(logger)  {}

void MBXServer::ensureConfigFile() {
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

void MBXServer::configureAccessPointRoutes(NetworkPortal * portal) {
    _logger->logDebug("MBXServer::configureAccessPointRoutes - begin");

    server->serveStatic("/", SPIFFS, "/")
            .setDefaultFile("/")
            .setCacheControl("no-store");

    server->on(Routes::ROOT, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *req) {
        serveSPIFFSFile(req, "/pages/configure_network.html", nullptr, HttpMediaTypes::HTML);
    });

    server->on(Routes::GET_SSID_LIST, WebRequestMethod::HTTP_GET, [portal](AsyncWebServerRequest *req) {
        req->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, MBXServerHandlers::getSsidListAsJson(portal->getSsidList()));
    });

    server->on(Routes::RESET_NETWORK, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *req) {
        serveSPIFFSFile(req, "/pages/reset_result.html", MBXServerHandlers::handleNetworkReset, HttpMediaTypes::HTML);
    }).setFilter(accessPointFilter);

    _logger->logDebug("MBXServer::configureAccessPointRoutes - end");
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
    }, MBXServerHandlers::handleUpload);;

    server->on(Routes::DOWNLOAD_CFG, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *req) {
        serveSPIFFSFile(req, "/conf/config.json", nullptr, HttpMediaTypes::JSON);
    });

    server->on(Routes::DOWNLOAD_CFG_EX, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *req) {
        serveSPIFFSFile(req, "/conf/example.json", nullptr, HttpMediaTypes::JSON);
    });

    server->on(Routes::RESET_NETWORK, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *req) {
       serveSPIFFSFile(req, "/pages/reset_result.html", MBXServerHandlers::handleNetworkReset,HttpMediaTypes::HTML);
    }).setFilter(accessPointFilter);

    _logger->logDebug("MBXServer::configurePageRoutes - end");
}

void MBXServer::begin() {
    _logger->logDebug("MBXServer::begin - begin");
    if (!SPIFFS.begin(true)) {
        _logger->logError("An error occurred while mounting SPIFFS");
        return;
    }

    ensureConfigFile();

    if(tryConnectWithStoredCreds()) {
        configurePageRoutes();
        server->begin();
    }
    else {
        NetworkPortal portal(_logger, _dns);
        configureAccessPointRoutes(&portal);
        server->begin();
        portal.begin();
    }

    _logger->logInformation("Web server started successfully");
    _logger->logDebug("MBXServer::begin - end");
}
auto MBXServer::accessPointFilter(AsyncWebServerRequest *request) -> bool {

    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == nullptr) {
        return true; // if unknown, let route be visible (same behavior as "not equal")
    }
    esp_netif_ip_info_t sta_ip{};
    if (esp_netif_get_ip_info(sta_netif, &sta_ip) != ESP_OK) {
        return true;
    }
    IPAddress staAddr(sta_ip.ip.addr);
    return staAddr != request->client()->localIP();
}

auto MBXServer::tryConnectWithStoredCreds() -> bool{
    wifi_mode_t mode{};
    esp_err_t e = esp_wifi_get_mode(&mode);
    if (e == ESP_ERR_WIFI_NOT_INIT) {
        // Netif/event loop (idempotent)
        ESP_ERROR_CHECK(esp_netif_init());
        (void)esp_event_loop_create_default();
        if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr) {
            ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_FAIL);
        }
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }

    // Check if stored STA config exists
    wifi_config_t sta_cfg{};
    if (esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) != ESP_OK || sta_cfg.sta.ssid[0] == '\0') {
        _logger->logInformation("No stored WiFi credentials; starting AP portal");
        return false;
    }

    // Start driver if not started
    wifi_bandwidth_t bw{};
    e = esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
    if (e == ESP_ERR_WIFI_NOT_STARTED || e == ESP_ERR_WIFI_NOT_INIT) {
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    // Ensure STA mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Apply stored config (safe to reapply)
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    // Connect and wait for IP
    ESP_ERROR_CHECK(esp_wifi_connect());

    const unsigned long start = millis();
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    while ((millis() - start) < WIFI_CONNECT_TIMEOUT) {
        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK &&
            ip.ip.addr != 0) {
            _logger->logInformation("Connected to WiFi using stored credentials");
            return true;
        }
        delay(WIFI_CONNECT_DELAY_MS);
    }
    _logger->logWarning("Failed to connect with stored credentials; starting AP portal");
    return false;
}

void MBXServer::serveSPIFFSFile(AsyncWebServerRequest* reqPtr, const char* path, std::function<void()> onServed, const char* contentType ){

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
        reqPtr->send(HttpResponseCodes::NOT_FOUND, HttpMediaTypes::PLAIN_TEXT,"Page not found");
    }
}