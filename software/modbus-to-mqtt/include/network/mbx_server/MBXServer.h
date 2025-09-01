#ifndef CONFIG_WEBSERVER_H
#define CONFIG_WEBSERVER_H

#include <DNSServer.h>
#include "ESPAsyncWebServer.h"
#include "Logger.h"
#include <ArduinoJson.h>

static constexpr int serverPort = 80;

class MBXServer {
public:
    explicit MBXServer(AsyncWebServer *server, DNSServer *dnsServer, Logger *logger);

    void begin() const;

    static void loop();

private:
    Logger *_logger;
    AsyncWebServer *server;
    DNSServer *_dnsServer;

    void configureRoutes() const;

    void ensureConfigFile() const;

    static auto safeWriteFile(FS &fs, const char *path, const String &content) -> bool;

    auto readConfig() const -> String;

    static void streamSPIFFSFileChunked(AsyncWebServerRequest *req, const char *path, const char *contentType) ;

    static auto accessPointFilter(AsyncWebServerRequest *request) -> bool;

    void configureAccessPointRoutes() const;

    auto tryConnectWithStoredCreds() const -> bool;

    static void serveSPIFFSFile(AsyncWebServerRequest *reqPtr, const char *path, const std::function<void()> &onServed,
                                const char *contentType, const Logger *logger);

    void logRequest(const AsyncWebServerRequest *request) const;
};

#endif
