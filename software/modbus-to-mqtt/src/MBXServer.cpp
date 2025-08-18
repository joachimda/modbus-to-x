#include "MBXServer.h"
#include "Logger.h"
#include "constants/HttpResponseCodes.h"
#include "constants/HttpMediaTypes.h"
#include "constants/ApiRoutes.h"
#include <ESPAsyncWebServer.h>

MBXServer::MBXServer(Logger * logger) : server(serverPort), _logger(logger) {}

void MBXServer::ensureConfigFile() {
    if (!SPIFFS.begin(true)) {

        _logger->logError("MBXServer::ensureConfigFile - File System error");
        return;
    }

    if (!SPIFFS.exists("/config.json")) {
        File file = SPIFFS.open("/config.json", FILE_WRITE);
        if (!file) {
            return;
        }

        file.print("{}");
        file.close();
    }
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

void MBXServer::configureRoutes() {

    _logger->logDebug("MBXServer::configureRoutes - begin");
    server.on(ApiRoutes::ROOT, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/index.html", HttpMediaTypes::HTML);
    });

    server.on(ApiRoutes::CONFIGURE, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/configure.html", HttpMediaTypes::HTML);
    });
    server.on(ApiRoutes::UPLOAD, WebRequestMethod::HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(HttpResponseCodes::OK, HttpMediaTypes::PLAIN_TEXT, "Upload OK");
    }, handleUpload);


    server.on(ApiRoutes::DOWNLOAD_CFG, WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
        if (SPIFFS.exists("/config.json")) {
            request->send(SPIFFS, "/config.json", HttpMediaTypes::JSON);
        } else {
            request->send(HttpResponseCodes::NOT_FOUND, HttpMediaTypes::PLAIN_TEXT,"No config file found!");
        }
    });
    server.on(ApiRoutes::DOWNLOAD_CFG_EX,
              WebRequestMethod::HTTP_GET,
              [](AsyncWebServerRequest *req) {
                  if(!SPIFFS.begin(true)) {
                      req->send(HttpResponseCodes::SERVER_ERROR, HttpMediaTypes::PLAIN_TEXT,"Filesystem Error");
                      return; }
                  if (SPIFFS.exists("/config_example.json")) {
                      req->send(SPIFFS, "/config_example.json", HttpMediaTypes::JSON);
                  } else {
                      req->send(HttpResponseCodes::NOT_FOUND, HttpMediaTypes::PLAIN_TEXT,"No config file found!");
                  }
              });

    _logger->logDebug("MBXServer::configureRoutes - end");
}

void MBXServer::begin() {

    _logger->logDebug("MBXServer::begin - begin");
    if (!SPIFFS.begin(true)) {
        _logger->logError("An error occurred while mounting SPIFFS");
        return;
    }

    configureRoutes();

    server.begin();
    _logger->logInformation("Web server started successfully");
    _logger->logDebug("MBXServer::begin - end");
}
