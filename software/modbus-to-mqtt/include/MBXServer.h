#ifndef CONFIG_WEBSERVER_H
#define CONFIG_WEBSERVER_H

#include <SPIFFS.h>
#include "ESPAsyncWebServer.h"

class MBXServer {
public:
    MBXServer();

    void begin();

    void configureRoutes();

private:
    AsyncWebServer server;

    static String readConfig();

    static void writeConfig(const String &json);

    static void handleUpload(AsyncWebServerRequest *request, String fn, size_t index,
                  uint8_t *data, size_t len, bool final);
};

#endif
