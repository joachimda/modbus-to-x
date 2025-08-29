#ifndef CONFIG_WEBSERVER_H
#define CONFIG_WEBSERVER_H

#include <SPIFFS.h>
#include <DNSServer.h>
#include "ESPAsyncWebServer.h"
#include "Logger.h"

static constexpr int serverPort = 80;
class MBXServer {
public:
    explicit MBXServer(AsyncWebServer * server, DNSServer * dns, Logger * logger);
    void begin() const;
private:
    Logger * _logger;
    AsyncWebServer * server;
    DNSServer * _dns;
    void configurePageRoutes() const;
    void ensureConfigFile() const;

    static auto safeWriteFile(FS &fs, const char *path, const String &content) -> bool;

    auto readConfig() const -> String;

    static auto accessPointFilter(AsyncWebServerRequest *request) -> bool;

    void configureAccessPointRoutes() const;

    auto tryConnectWithStoredCreds() const -> bool;

    static void serveSPIFFSFile(AsyncWebServerRequest *reqPtr, const char *path, std::function<void()> onServed,
                         const char *contentType);
};

#endif
