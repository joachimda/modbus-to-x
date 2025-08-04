#ifndef CONFIG_WEBSERVER_H
#define CONFIG_WEBSERVER_H

#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "commlink/CommLink.h" // For accessing CommLink MQTT configuration (if needed)

class MBXServer {
private:
    AsyncWebServer server;       // Web server instance
    CommLink *commLink;          // Reference to CommLink for fetching/storing configurations

    // Helper function to read JSON configuration from `/config.json`
    String readConfig() {
        if (!SPIFFS.exists("/config.json")) {
            return "{}"; // Return empty JSON if file does not exist
        }

        File file = SPIFFS.open("/config.json", "r");
        String json = file.readString();
        file.close();
        return json;
    }

    // Helper function to write JSON configuration to `/config.json`
    void writeConfig(const String& json) {
        File file = SPIFFS.open("/config.json", "w");
        if (file) {
            file.print(json);
            file.close();
        }
    }

public:
    // Constructor
    MBXServer(CommLink *link)
            : server(80), commLink(link) {}

    // Starts the web server
    void begin() {
        // Initialize SPIFFS
        if (!SPIFFS.begin(true)) {
            Serial.println("An error occurred while mounting SPIFFS");
            return;
        }

        // Configure routes
        configureRoutes();

        // Start the server
        server.begin();
        Serial.println("Web server started successfully");
    }

    // Configures the web server routes
    void configureRoutes() {
        // Serve HTML page for uploading/downloading configuration
        server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
            String html = R"rawliteral(
                <!DOCTYPE html>
                <html>
                <body>
                    <h1>ESP Configuration</h1>
                    <form action="/upload" method="post" enctype="multipart/form-data">
                        Select JSON file to upload: <br>
                        <input type="file" name="config"><br><br>
                        <input type="submit" value="Upload JSON">
                    </form>
                    <h3>Download Current Configuration</h3>
                    <a href="/download" target="_blank">Download JSON</a>
                </body>
                </html>
            )rawliteral";
            request->send(200, "text/html", html);
        });

        // Upload JSON configuration
        server.onFileUpload([this](AsyncWebServerRequest *request, const String& filename,
                                   size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) { // Start of a new upload
                File file = SPIFFS.open("/config.json", "w");
                if (file) {
                    file.close();
                }
            }

            // Append data to the file
            File file = SPIFFS.open("/config.json", "a");
            if (file) {
                file.write(data, len);
                file.close();
            }

            if (final) { // When file upload is complete
                // Optionally, parse the uploaded JSON to update CommLink's settings
                if (commLink) {
                    File file = SPIFFS.open("/config.json", "r");
                    if (file) {
                        String json = file.readString();
                        file.close();

                        // Parse JSON and update CommLink's configuration
                        DynamicJsonDocument doc(1024);
                        DeserializationError error = deserializeJson(doc, json);
                        if (!error) {
                            if (doc.containsKey("mqtt_broker") && doc.containsKey("mqtt_port")) {
                                String broker = doc["mqtt_broker"];
                                String port = doc["mqtt_port"];
                                commLink->updateMqttConfig(broker.c_str(), port.c_str());
                                Serial.println("CommLink configuration updated");
                            }
                        }
                    }
                }

                request->send(200, "text/plain", "File uploaded successfully!");
            }
        });

        // Download existing JSON configuration
        server.on("/download", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (SPIFFS.exists("/config.json")) {
                request->send(SPIFFS, "/config.json", "application/json");
            } else {
                request->send(404, "text/plain", "No configuration file found!");
            }
        });
    }
};

#endif