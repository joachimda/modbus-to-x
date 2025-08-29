#include "network/mbx_server/MBXServerHandlers.h"
#include <atomic>
#include <SPIFFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "network/NetworkPortal.h"

namespace {
    std::atomic<NetworkPortal *> g_portal{nullptr};

    bool parseConnectPayload(uint8_t *data, size_t len,
                             String &ssid, String &pass, String &bssid, bool &save, WifiStaticCfg &st, uint8_t &channel) {
Serial.print("MBXServerHandlers::parseConnectPayload called with: ");
        Serial.println("data: " + String(data, len));
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) return false;

        ssid = doc["ssid"] | "";
        pass = doc["password"] | "";
        bssid = doc["bssid"] | "";
        save = doc["save"] | true;
        channel = static_cast<uint8_t>(doc["channel"] | 0);

        if (doc["static"].is<JsonObject>()) {
            const auto s = doc["static"].as<JsonObject>();
            st.ip = s["ip"] | "";
            st.gateway = s["gateway"] | "";
            st.subnet = s["subnet"] | s["mask"] | "";
            st.dns1 = s["dns1"] | "";
            st.dns2 = s["dns2"] | "";
        } else {
            st = {};
        }
        return true;
    }

    const char *stateToStr(WifiConnState s) {
        switch (s) {
            case WifiConnState::Idle: return "idle";
            case WifiConnState::Connecting: return "connecting";
            case WifiConnState::Connected: return "connected";
            case WifiConnState::Failed: return "failed";
            case WifiConnState::Disconnected: return "disconnected";
        }
        return "unknown";
    }

    void sendJson(AsyncWebServerRequest *req, const JsonDocument &doc, int code = 200) {
        String out;
        serializeJson(doc, out);
        req->send(code, "application/json", out);
    }
}

void MBXServerHandlers::setPortal(NetworkPortal *portal) {
    g_portal.store(portal, std::memory_order_release);
}

void MBXServerHandlers::getSsidListAsJson(AsyncWebServerRequest *req) {
    auto *portal = g_portal.load(std::memory_order_acquire);
    if (!portal) {
        req->send(503, "application/json", "[]");
        return;
    }

    auto snap = portal->getLatestScanResultsSnapshot();

    String out;
    out.reserve(snap ? (snap->size() * 64 + 2) : 2);
    out += "[";

    if (snap && !snap->empty()) {
        for (size_t i = 0; i < snap->size(); ++i) {
            const auto &ap = (*snap)[i];

            String escSsid;
            escSsid.reserve(ap.SSID.length() + 4);
            for (char c: ap.SSID) {
                if (c == '\\' || c == '\"') {
                    escSsid += '\\';
                    escSsid += c;
                } else if (static_cast<uint8_t>(c) < 0x20) { escSsid += ' '; } else { escSsid += c; }
            }

            char bssidBuf[18] = {0};
            if (ap.hasBSSID) {
                snprintf(bssidBuf, sizeof(bssidBuf),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         ap.BSSID[0], ap.BSSID[1], ap.BSSID[2],
                         ap.BSSID[3], ap.BSSID[4], ap.BSSID[5]);
            }

            const bool isOpen = (ap.encryptionType == WIFI_AUTH_OPEN);
            const char* authName = isOpen ? "OPEN" :
              (ap.encryptionType == WIFI_AUTH_WEP ? "WEP" :
              (ap.encryptionType == WIFI_AUTH_WPA_PSK ? "WPA" :
              (ap.encryptionType == WIFI_AUTH_WPA2_PSK ? "WPA2" :
              (ap.encryptionType == WIFI_AUTH_WPA_WPA2_PSK ? "WPA/WPA2" :
              (ap.encryptionType == WIFI_AUTH_WPA2_ENTERPRISE ? "WPA2-ENT" :
              (ap.encryptionType == WIFI_AUTH_WPA3_PSK ? "WPA3" : "UNKNOWN"))))));

            out += R"({"ssid":")"; out += escSsid; out += "\"";
            out += ",\"rssi\":";   out += String((long)ap.RSSI);
            out += R"(,"secure":)"; out += (ap.encryptionType == WIFI_AUTH_OPEN ? "false" : "true");
            out += R"(,"auth":")";  out += authName; out += "\"";
            out += ",\"channel\":"; out += String((unsigned)ap.channel);
            if (ap.hasBSSID) { out += R"(,"bssid":")"; out += bssidBuf; out += "\""; }
            out += "}";

            if (i + 1 < snap->size()) out += ",";
        }
    }

    out += "]";
    req->send(200, "application/json", out);
}

void MBXServerHandlers::handleNetworkReset() {
    WiFi.persistent(true);
    WiFi.disconnect(true, true);
    WiFi.persistent(false);
    WiFiClass::mode(WIFI_MODE_APSTA);
}

void MBXServerHandlers::handleUpload(AsyncWebServerRequest *r, const String &fn, size_t index, uint8_t *data,
                                     size_t len, bool final) {
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

void MBXServerHandlers::handleWifiConnect(AsyncWebServerRequest* req, WiFiConnectController& wifi,
                                          uint8_t* data, size_t len, size_t index, size_t total)
{
    // Accumulate chunks (ESPAsyncWebServer can split large bodies)
    static String body;
    if (index == 0) body = "";
    body.concat((const char*)data, len);
    if (index + len < total) return; // wait for last chunk

    String ssid, pass, bssid;
    uint8_t channel = 0;
    bool save = true;
    WifiStaticCfg st;

    if (!parseConnectPayload((uint8_t*)body.c_str(), body.length(), ssid, pass, bssid, save, st, channel)
        || ssid.isEmpty()) {
        req->send(400, "application/json", "{\"error\":\"bad_request\"}");
        return;
    }

    bool accepted = wifi.connect(ssid, pass, bssid, st, save, channel);
    if (!accepted) {
        req->send(409, "application/json", "{\"error\":\"already_connecting\"}");
        return;
    }
    if (auto* p = g_portal.load(std::memory_order_acquire)) {
        p->suspendScanning(true);
    }

    // 202 Accepted keeps the UI simple (it will poll /status)
    req->send(202, "application/json", "{\"ok\":true}");
}

void MBXServerHandlers::handleWifiStatus(AsyncWebServerRequest* req, WiFiConnectController& wifi) {
    WifiStatus s = wifi.status();
    if (s.state == WifiConnState::Connected || s.state == WifiConnState::Failed || s.state == WifiConnState::Disconnected) {
        if (auto* p = g_portal.load(std::memory_order_acquire)) {
            p->suspendScanning(false);
        }
    }
    JsonDocument doc;
    doc["state"]  = stateToStr(s.state);
    doc["ssid"]   = s.ssid;
    if (s.hasIp) doc["ip"] = s.ip;
    if (s.reason.length()) doc["reason"] = s.reason;

    sendJson(req, doc);
}

void MBXServerHandlers::handleWifiCancel(AsyncWebServerRequest* req, WiFiConnectController& wifi) {
    wifi.cancel();
    req->send(200, "application/json", "{\"ok\":true}");
}

void MBXServerHandlers::handleWifiApOff(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"ok\":true}");
    if (auto* p = g_portal.load(std::memory_order_acquire)) p->stop();
    // defer mode change so the response reaches the client
    xTaskCreatePinnedToCore([](void*){
      delay(800);
      WiFiClass::mode(WIFI_MODE_STA);
      vTaskDelete(nullptr);
    }, "apOff", 2048, nullptr, 1, nullptr, APP_CPU_NUM);
}