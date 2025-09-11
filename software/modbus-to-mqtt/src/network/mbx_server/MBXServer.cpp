#include "network/mbx_server/MBXServer.h"
#include "Logger.h"
#include "constants/HttpResponseCodes.h"
#include "constants/HttpMediaTypes.h"
#include "constants/Routes.h"
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "network/NetworkPortal.h"
#include "network/mbx_server/MBXServerHandlers.h"
#include "services/IndicatorService.h"
#include "services/ArduinoOtaManager.h"

static constexpr auto WIFI_CONNECT_DELAY_MS = 100;
static constexpr auto WIFI_CONNECT_TIMEOUT = 30000;
static WifiConnectionController g_wifi;

MBXServer::MBXServer(AsyncWebServer *server, DNSServer *dnsServer, Logger *logger) : _logger(logger), server(server),
    _dnsServer(dnsServer) {
}

void MBXServer::begin() const {
    _logger->logDebug("MBXServer::begin - begin");
    ensureConfigFile();
    if (tryConnectWithStoredCreds()) {
        configureRoutes();
        server->begin();
        IndicatorService::instance().setPortalMode(false);
        IndicatorService::instance().setWifiConnected(true);
        // Enable MQTT in normal (non-portal) mode
        MqttManager::setMQTTEnabled(true);
        ArduinoOtaManager::begin(_logger);
    } else {
        g_wifi.begin("modbus-to-x");

        NetworkPortal portal(_logger, _dnsServer);
        MBXServerHandlers::setPortal(&portal);
        configureAccessPointRoutes();
        server->begin();
        IndicatorService::instance().setPortalMode(true);
        // Disable MQTT while portal is active to avoid transient connects
        MqttManager::setMQTTEnabled(false);
        portal.begin();
    }

    _logger->logDebug("MBXServer::begin - end");
}

void MBXServer::loop() {
    g_wifi.loop();
    // Keep LED_A in sync with Wi-Fi status when not in portal mode
    IndicatorService::instance().setWifiConnected(WiFiClass::status() == WL_CONNECTED);
    ArduinoOtaManager::loop();
}

void MBXServer::configureRoutes() const {
    _logger->logDebug("MBXServer::configureRoutes - begin");
    server->serveStatic("/", SPIFFS, Routes::ROOT)
            .setDefaultFile("index.html")
            .setCacheControl("no-store");

    server->on(Routes::CONFIGURE, HTTP_GET, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        req->send(SPIFFS, "/pages/configure.html", HttpMediaTypes::HTML);
    });

    server->on("/__stream/index", HTTP_GET, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        streamSPIFFSFileChunked(req, "/index.html", HttpMediaTypes::HTML);
    });

    server->on(Routes::PUT_MODBUS_CONFIG, HTTP_PUT, [this](AsyncWebServerRequest *req) {
        logRequest(req);
    }, nullptr,[](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        MBXServerHandlers::handlePutModbusConfigBody(req, data, len, index, total);
    });

    server->on(Routes::GET_MODBUS_CONFIG, HTTP_GET, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        serveSPIFFSFile(req, "/conf/config.json", nullptr, HttpMediaTypes::JSON, _logger);
    });

    server->on(Routes::PUT_MQTT_CONFIG, HTTP_PUT, [this](AsyncWebServerRequest *req) {
        logRequest(req);
    }, nullptr,[](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        MBXServerHandlers::handlePutMqttConfigBody(req, data, len, index, total);
    });

    server->on(Routes::PUT_MQTT_SECRET, HTTP_POST, [this](AsyncWebServerRequest *req) {
        logRequest(req);
    }, nullptr,[](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        MBXServerHandlers::handlePutMqttSecretBody(req, data, len, index, total);
    });

    server->on(Routes::GET_MQTT_CONFIG, HTTP_GET, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        serveSPIFFSFile(req, "/conf/mqtt.json", nullptr, HttpMediaTypes::JSON, _logger);
    });

    server->on(Routes::LOGS, HTTP_GET, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        MBXServerHandlers::getLogs(req);
    });

    server->on(Routes::RESET_NETWORK, HTTP_GET, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        serveSPIFFSFile(req, "/pages/reset_result.html",
                        MBXServerHandlers::handleNetworkReset,
                        HttpMediaTypes::HTML,
                        _logger);
    });

    server->on(Routes::SYSTEM_STATS, HTTP_GET, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        MBXServerHandlers::getSystemStats(req, _logger);
    });

    server->on(Routes::MQTT_TEST_CONNECT, HTTP_POST, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        MBXServerHandlers::handleMqttTestConnection(req);
    });

    server->on(Routes::OTA_FIRMWARE, HTTP_POST, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        if (!req->authenticate(OTA_HTTP_USER, OTA_HTTP_PASS)) { req->requestAuthentication(); }
    }, [this](AsyncWebServerRequest *req, const String &fn, const size_t index, uint8_t *data, const size_t len, const bool final) {
        MBXServerHandlers::handleOtaFirmwareUpload(req, fn, index, data, len, final, _logger);
    });

    server->on(Routes::OTA_FILESYSTEM, HTTP_POST, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        if (!req->authenticate(OTA_HTTP_USER, OTA_HTTP_PASS)) { req->requestAuthentication(); }
    }, [this](AsyncWebServerRequest *req, const String &fn, const size_t index, uint8_t *data, const size_t len, bool final) {
        MBXServerHandlers::handleOtaFilesystemUpload(req, fn, index, data, len, final, _logger);
    });
    server->onNotFound([this](AsyncWebServerRequest *req) {
        logRequest(req);
        req->send(HttpResponseCodes::NOT_FOUND, HttpMediaTypes::PLAIN_TEXT, "I haz no file");
    });
    server->on(Routes::DEVICE_RESET, HTTP_POST, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        MBXServerHandlers::handleDeviceReset(_logger);
    });

    _logger->logDebug("MBXServer::configureRoutes - end");
}

void MBXServer::configureAccessPointRoutes() const {
    _logger->logDebug("MBXServer::configureAccessPointRoutes - begin");

    server->serveStatic("/", SPIFFS, "/")
            .setDefaultFile("/pages/configure_network.html")
            .setCacheControl("no-store");

    server->on(Routes::ROOT, HTTP_GET, [this](AsyncWebServerRequest *req) {
        serveSPIFFSFile(req, "/pages/configure_network.html", nullptr, HttpMediaTypes::HTML, _logger);
    });

    server->on(Routes::GET_SSID_LIST, HTTP_GET, [](AsyncWebServerRequest *req) {
        MBXServerHandlers::getSsidListAsJson(req);
    });

    server->on(Routes::POST_WIFI_CONNECT, HTTP_POST,
               [this](const AsyncWebServerRequest *req) {
                   logRequest(req);
               },
               nullptr,
               [](AsyncWebServerRequest *req, const uint8_t *data, const size_t len, const size_t index, const size_t total) {
                   MBXServerHandlers::handleWifiConnect(req, g_wifi, data, len, index, total);
               }
    );

    server->on(Routes::RESET_NETWORK, HTTP_GET, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        serveSPIFFSFile(req, "/pages/reset_result.html", MBXServerHandlers::handleNetworkReset, HttpMediaTypes::HTML,
                        _logger);
    }).setFilter(accessPointFilter);

    server->on(Routes::GET_WIFI_STATUS, HTTP_GET, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        MBXServerHandlers::handleWifiStatus(req, g_wifi);
    });
    server->on(Routes::POST_WIFI_AP_OFF, HTTP_POST, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        MBXServerHandlers::handleWifiApOff(req);
    });
    server->on(Routes::POST_WIFI_CANCEL, HTTP_POST, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        MBXServerHandlers::handleWifiCancel(req, g_wifi);
    });

    server->on(Routes::OTA_FIRMWARE, HTTP_POST, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        if (!req->authenticate(OTA_HTTP_USER, OTA_HTTP_PASS)) { req->requestAuthentication(); }
    }, [this](AsyncWebServerRequest *req, const String &fn, const size_t index, uint8_t *data, const size_t len, const bool final) {
        MBXServerHandlers::handleOtaFirmwareUpload(req, fn, index, data, len, final, _logger);
    });
    server->on(Routes::OTA_FILESYSTEM, HTTP_POST, [this](AsyncWebServerRequest *req) {
        logRequest(req);
        if (!req->authenticate(OTA_HTTP_USER, OTA_HTTP_PASS)) { req->requestAuthentication(); }
    }, [this](AsyncWebServerRequest *req, const String &fn, const size_t index, uint8_t *data, const size_t len, const bool final) {
        MBXServerHandlers::handleOtaFilesystemUpload(req, fn, index, data, len, final, _logger);
    });
    _logger->logDebug("MBXServer::configureAccessPointRoutes - end");
}

auto MBXServer::accessPointFilter(AsyncWebServerRequest *request) -> bool {
    // True when the HTTP connection is to this AP interface,
    const IPAddress dest = request->client()->localIP();
    return dest == WiFi.softAPIP();
}

auto MBXServer::tryConnectWithStoredCreds() const -> bool {
    _logger->logDebug("MBXServer::tryConnectWithStoredCreds - begin");

    if (WiFiClass::getMode() != WIFI_MODE_STA) {
        WiFiClass::mode(WIFI_MODE_STA);
        delay(300);
    }

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
    _logger->logInformation(
        "MBXServer::tryConnectWithStoredCreds() - No connection with stored credentials; starting AP portal");
    return false;
}

void MBXServer::serveSPIFFSFile(AsyncWebServerRequest *reqPtr, const char *path, const std::function<void()> &onServed,
                                const char *contentType, const Logger *logger) {
    if (SPIFFS.exists(path)) {
        logger->logDebug(("Serving file: " + String(path)).c_str());
        reqPtr->send(SPIFFS, path, contentType);
        if (onServed) {
            reqPtr->onDisconnect([onServed, logger]() {
                logger->logDebug("Calling onServed (deferred)");
                onServed();
                logger->logDebug("onServed called (deferred)");
            });
        }
    } else {
        logger->logDebug(("File not found: " + String(path)).c_str());
        reqPtr->send(HttpResponseCodes::NOT_FOUND, HttpMediaTypes::PLAIN_TEXT, "Page not found");
    }
}

void MBXServer::logRequest(const AsyncWebServerRequest *request) const {
    _logger->logDebug(
        ("MBXServer: - Processing request: " + String(request->methodToString()) + ": " + String(request->url())).
        c_str());
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
    if (!SPIFFS.exists("/conf/mqtt.json")) {
        _logger->logWarning("MBXServer::ensureConfigFile - MQTT config file not found. Creating new one");
        File file = SPIFFS.open("/conf/mqtt.json", FILE_WRITE);
        if (file) {
            file.print("{\"broker_ip\":\"0.0.0.0\",\"broker_url\":\"\",\"broker_port\":\"1883\",\"user\":\"\",\"root_topic\":\"mbx_root\"}");
            file.close();
        }
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

void MBXServer::streamSPIFFSFileChunked(AsyncWebServerRequest *req, const char *path, const char *contentType) {
    if (!SPIFFS.exists(path)) {
        req->send(HttpResponseCodes::NOT_FOUND, HttpMediaTypes::PLAIN_TEXT, "Page not found");
        return;
    }

    auto filePtr = std::make_shared<File>(SPIFFS.open(path, FILE_READ));
    if (!filePtr || !(*filePtr)) {
        req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::PLAIN_TEXT, "File open failed");
        return;
    }

    auto *response = req->beginChunkedResponse(contentType,
                                               [filePtr](uint8_t *buffer, const size_t maxLen, size_t) -> size_t {
                                                   if (!*filePtr) return 0;
                                                   const size_t n = filePtr->read(buffer, maxLen);
                                                   // Returning 0 signals end-of-stream; shared_ptr will release and close
                                                   return n;
                                               });

    response->addHeader("Cache-Control", "no-store");
    req->send(response);
}
