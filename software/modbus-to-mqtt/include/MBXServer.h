#ifndef CONFIG_WEBSERVER_H
#define CONFIG_WEBSERVER_H

#include <SPIFFS.h>
#include "ESPAsyncWebServer.h"
#include "Logger.h"

static const int serverPort = 80;
class MBXServer {
public:
    explicit MBXServer(Logger * logger);

    void begin();

    void configureRoutes();

private:
    Logger * _logger;
    AsyncWebServer server;

    void ensureConfigFile();
    String readConfig();

    static void handleUpload(AsyncWebServerRequest *r, const String& fn, size_t index,
                             uint8_t *data, size_t len, bool final);
};

#endif
