#include "MBXServer.h"
#include <ESPAsyncWebServer.h>

MBXServer::MBXServer() : server(80) {}

void MBXServer::handleUpload(AsyncWebServerRequest *request, String fn, size_t index, uint8_t *data, size_t len, bool final) {
    static File uploadFile;
    if (!index) {
        uploadFile = SPIFFS.open("/config.json", "w");
    }
    if (uploadFile) {
        uploadFile.write(data, len);
    }
    if (final && uploadFile) {
        uploadFile.close();
    }
}

void MBXServer::writeConfig(const String &json) {
    File file = SPIFFS.open("/config.json", "w");
    if (file) {
        file.print(json);
        file.close();
    }
}

String MBXServer::readConfig() {
    if (!SPIFFS.exists("/config.json")) {
        return "{}";
    }

    File file = SPIFFS.open("/config.json", "r");
    String json = file.readString();
    file.close();
    return json;
}

void MBXServer::configureRoutes() {
    server.on("/", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/index.html", "text/html");
    });

    server.on("/config", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/modbus_config.json", "application/json");
    });
    server.on("/upload", WebRequestMethod::HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "Upload successful");
    }, handleUpload);


    server.on("/download", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
        if (SPIFFS.exists("/config.json")) {
            request->send(SPIFFS, "/config.json", "application/json");
        } else {
            request->send(404, "text/plain", "No configuration file found!");
        }
    });
}

void MBXServer::begin() {
    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("An error occurred while mounting SPIFFS");
        return;
    }

    configureRoutes();

    server.begin();
    Serial.println("Web server started successfully");
}
