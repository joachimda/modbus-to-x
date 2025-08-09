#ifndef CONFIG_WEBSERVER_H
#define CONFIG_WEBSERVER_H

#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>

class MBXServer {
public:
    explicit MBXServer()
        : server(80) {
    }

    void begin() {
        // Initialize SPIFFS
        if (!SPIFFS.begin(true)) {
            Serial.println("An error occurred while mounting SPIFFS");
            return;
        }

        configureRoutes();

        server.begin();
        Serial.println("Web server started successfully");
    }

    void configureRoutes() {
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
            request->send(SPIFFS, "/index.html", "text/html");
        });

        server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
            request->send(SPIFFS, "/modbus_config.json", "application/json");
        });
        server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
          request->send(200, "text/plain", "Upload successful");
        }, handleUpload);


        server.on("/download", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (SPIFFS.exists("/config.json")) {
                request->send(SPIFFS, "/config.json", "application/json");
            } else {
                request->send(404, "text/plain", "No configuration file found!");
            }
        });
    }

private:
    AsyncWebServer server;

    static String readConfig() {
        if (!SPIFFS.exists("/config.json")) {
            return "{}";
        }

        File file = SPIFFS.open("/config.json", "r");
        String json = file.readString();
        file.close();
        return json;
    }

    static void writeConfig(const String &json) {
        File file = SPIFFS.open("/config.json", "w");
        if (file) {
            file.print(json);
            file.close();
        }
    }

    void handleUpload(AsyncWebServerRequest *request, String fn, size_t index,
                  uint8_t *data, size_t len, bool final) {
        static File uploadFile;

        if (!index) {
            uploadFile = SPIFFS.open("/modbus_config.json", "w");
        }
        if (uploadFile) {
            uploadFile.write(data, len);
        }
        if (final && uploadFile) {
            uploadFile.close();
        }
    }
};

#endif
