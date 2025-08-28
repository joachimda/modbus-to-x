#ifndef CONFIG_WEBSERVER_H
#define CONFIG_WEBSERVER_H

#include <SPIFFS.h>
#include <DNSServer.h>
#include "ESPAsyncWebServer.h"
#include "Logger.h"
#include "wifiManagement/NetworkPortal.h"

static const int serverPort = 80;
class MBXServer {
public:
    explicit MBXServer(AsyncWebServer * server, DNSServer * dns, Logger * logger);
    void begin();
private:
    Logger * _logger;
    AsyncWebServer * server;
    DNSServer * _dns;
    void configurePageRoutes();
    void ensureConfigFile();
    auto readConfig() -> String;

    static auto accessPointFilter(AsyncWebServerRequest *request) -> bool;

    void configureAccessPointRoutes(NetworkPortal * portal);

    auto tryConnectWithStoredCreds() -> bool;

    static void serveSPIFFSFile(AsyncWebServerRequest *reqPtr, const char *path, std::function<void()> onServed,
                         const char *contentType);
};

#endif
