#include "network/mbx_server/MBXServerHandlers.h"
#include <atomic>
#include <SPIFFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "constants/HttpMediaTypes.h"
#include "constants/HttpResponseCodes.h"
#include "network/NetworkPortal.h"
#include "services/StatService.h"
#include "services/OtaService.h"
#include "services/IndicatorService.h"

auto constexpr OTA_FS_UPLOAD_BEGIN_FAIL_RESP = R"({"error":"ota_begin_failed"})";
auto constexpr OTA_FW_UPLOAD_BEGIN_FAIL_RESP = R"({"error":"ota_begin_failed"})";
auto constexpr OTA_END_FAIL_RESP = R"({"error":"ota_end_failed"})";
auto constexpr OTA_END_FW_UPLOAD_OK = R"({"ok":true,"type":"firmware"})";
auto constexpr OTA_END_FS_UPLOAD_OK = R"({"ok":true,"type":"filesystem"})";
auto constexpr BAD_REQUEST_RESP = R"({"error":"bad_request"})";
auto constexpr WIFI_HANDLER_OK_RESP = "{\"ok\":true}";
auto constexpr WIFI_ALREADY_CONNECTING_RESP = R"({"error":"already_connecting"})";

auto constexpr NETWORK_RESET_DELAY_MS = 5000;

std::atomic<NetworkPortal *> g_portal{nullptr};
static std::atomic<MemoryLogger *> g_memlog{nullptr};

bool parseConnectPayload(uint8_t *data, size_t len, String &ssid, String &pass,
                         String &bssid, bool &save, WifiStaticConfig &st,
                         uint8_t &channel) {
    Serial.print("MBXServerHandlers::parseConnectPayload called with: ");
    Serial.println("data: " + String(data, len));
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, data, len);
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

const char *stateToStr(const WifiConnectionState s) {
    switch (s) {
        case WifiConnectionState::Idle: return "idle";
        case WifiConnectionState::Connecting: return "connecting";
        case WifiConnectionState::Connected: return "connected";
        case WifiConnectionState::Failed: return "failed";
        case WifiConnectionState::Disconnected: return "disconnected";
    }
    return "unknown";
}

void sendJson(AsyncWebServerRequest *req, const JsonDocument &doc) {
    String out;
    serializeJson(doc, out);
    req->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, out);
}

void MBXServerHandlers::setPortal(NetworkPortal *portal) {
    g_portal.store(portal, std::memory_order_release);
}

void MBXServerHandlers::setMemoryLogger(MemoryLogger *mem) {
    g_memlog.store(mem, std::memory_order_release);
}

void MBXServerHandlers::getSsidListAsJson(AsyncWebServerRequest *req) {
    auto *portal = g_portal.load(std::memory_order_acquire);
    if (!portal) {
        req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::JSON, "[]");
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
            const char *authName = isOpen
                                       ? "OPEN"
                                       : (ap.encryptionType == WIFI_AUTH_WEP
                                              ? "WEP"
                                              : (ap.encryptionType == WIFI_AUTH_WPA_PSK
                                                     ? "WPA"
                                                     : (ap.encryptionType == WIFI_AUTH_WPA2_PSK
                                                            ? "WPA2"
                                                            : (ap.encryptionType == WIFI_AUTH_WPA_WPA2_PSK
                                                                   ? "WPA/WPA2"
                                                                   : (ap.encryptionType == WIFI_AUTH_WPA2_ENTERPRISE
                                                                          ? "WPA2-ENT"
                                                                          : (ap.encryptionType == WIFI_AUTH_WPA3_PSK
                                                                                 ? "WPA3"
                                                                                 : "UNKNOWN"))))));

            out += R"({"ssid":")";
            out += escSsid;
            out += "\"";
            out += ",\"rssi\":";
            out += String(static_cast<long>(ap.RSSI));
            out += R"(,"secure":)";
            out += (ap.encryptionType == WIFI_AUTH_OPEN ? "false" : "true");
            out += R"(,"auth":")";
            out += authName;
            out += "\"";
            out += ",\"channel\":";
            out += String(static_cast<unsigned>(ap.channel));
            if (ap.hasBSSID) {
                out += R"(,"bssid":")";
                out += bssidBuf;
                out += "\"";
            }
            out += "}";

            if (i + 1 < snap->size()) out += ",";
        }
    }

    out += "]";
    req->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, out);
}

void MBXServerHandlers::handleNetworkReset() {
    Serial.println("MBXServerHandlers::handleNetworkReset called");
    delay(NETWORK_RESET_DELAY_MS);
    WiFi.persistent(true);
    WiFi.disconnect(true, true);
    WiFi.persistent(false);
    WiFiClass::mode(WIFI_MODE_APSTA);
}

void MBXServerHandlers::handleUpload(AsyncWebServerRequest *r, const String &fn, const size_t index, const uint8_t *data,
                                     const size_t len, const bool final) {
    static File uploadFile;
    if (index == 0U) {
        uploadFile = SPIFFS.open("/conf/config.json", FILE_WRITE);
    }
    if (uploadFile) {
        uploadFile.write(data, len);
    }
    if (final && uploadFile) {
        uploadFile.close();
    }
}

void MBXServerHandlers::handlePutModbusConfigBody(AsyncWebServerRequest *req, const uint8_t *data, const size_t len,
                                                  const size_t index,
                                                  const size_t total) {
    static File bodyFile;
    if (index == 0U) {
        bodyFile = SPIFFS.open("/conf/config.json", FILE_WRITE);
    }
    if (bodyFile) {
        bodyFile.write(data, len);
    }
    if (index + len == total) {
        if (bodyFile) bodyFile.close();
        req->send(HttpResponseCodes::NO_CONTENT);
    }
}


void MBXServerHandlers::handleWifiConnect(AsyncWebServerRequest *req, WifiConnectionController &wifi,
                                          const uint8_t *data, const size_t len,
                                          const size_t index, const size_t total) {
    static String body;
    if (index == 0) body = "";
    body.concat(reinterpret_cast<const char *>(data), len);
    if (index + len < total) return;

    String ssid, pass, bssid;
    uint8_t channel = 0;
    bool save = true;
    WifiStaticConfig st;

    if (!parseConnectPayload((uint8_t *) body.c_str(), body.length(), ssid, pass, bssid, save, st, channel)
        || ssid.isEmpty()) {
        req->send(HttpResponseCodes::BAD_REQUEST, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        return;
    }

    const bool accepted = wifi.connect(ssid, pass, bssid, st, save, channel);
    if (!accepted) {
        req->send(HttpResponseCodes::CONFLICT, HttpMediaTypes::JSON, WIFI_ALREADY_CONNECTING_RESP);
        return;
    }
    if (auto *p = g_portal.load(std::memory_order_acquire)) {
        p->suspendScanning(true);
    }

    req->send(HttpResponseCodes::ACCEPTED, HttpMediaTypes::JSON, WIFI_HANDLER_OK_RESP);
}

void MBXServerHandlers::handleWifiStatus(AsyncWebServerRequest *req, const WifiConnectionController &wifi) {
    const WifiStatus s = wifi.getStatus();
    if (s.state == WifiConnectionState::Connected
        || s.state == WifiConnectionState::Failed
        || s.state == WifiConnectionState::Disconnected) {
        if (auto *p = g_portal.load(std::memory_order_acquire)) {
            p->suspendScanning(false);
        }
    }
    JsonDocument doc;
    doc["state"] = stateToStr(s.state);
    doc["ssid"] = s.ssid;
    if (s.hasIp) doc["ip"] = s.ip;
    if (s.reason.length()) doc["reason"] = s.reason;

    sendJson(req, doc);
}

void MBXServerHandlers::handleWifiCancel(AsyncWebServerRequest *req, WifiConnectionController &wifi) {
    wifi.cancel();
    req->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, WIFI_HANDLER_OK_RESP);
}

void MBXServerHandlers::handleWifiApOff(AsyncWebServerRequest *req) {
    req->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, WIFI_HANDLER_OK_RESP);
    if (auto *p = g_portal.load(std::memory_order_acquire)) p->stop();
    xTaskCreatePinnedToCore([](void *) {
        delay(800);
        WiFiClass::mode(WIFI_MODE_STA);
        IndicatorService::instance().setPortalMode(false);
        vTaskDelete(nullptr);
    }, "apOff", 2048, nullptr, 1, nullptr, APP_CPU_NUM);
}

void MBXServerHandlers::getSystemStats(AsyncWebServerRequest *req, const Logger *logger) {
    logger->logDebug(
        ("MBX Server: Started processing "
         + String(req->methodToString()) + " request on " + req->url()).c_str());

    JsonDocument doc;
    doc = StatService::appendSystemStats(doc);
    doc = StatService::appendModbusStats(doc);
    doc = StatService::appendMQTTStats(doc);
    doc = StatService::appendNetworkStats(doc);
    doc = StatService::appendStorageStats(doc);
    doc = StatService::appendHealthStats(doc);

    sendJson(req, doc);
    logger->logDebug(
        ("MBX Server: Finished processing " + String(req->methodToString()) + " request on " + req->url()).c_str());
}

void MBXServerHandlers::getLogs(AsyncWebServerRequest *req) {
    if (auto *mem = g_memlog.load(std::memory_order_acquire)) {
        const String text = mem->toText();
        req->send(HttpResponseCodes::OK, "text/plain; charset=utf-8", text);
    } else {
        req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::PLAIN_TEXT, "logging buffer unavailable");
    }
}

void MBXServerHandlers::handleDeviceReset(const Logger *logger) {
    logger->logInformation("Device reset requested. Will reset in 5 sec");
    delay(5000);
    ESP.restart();
}

void MBXServerHandlers::handleOtaFirmwareUpload(AsyncWebServerRequest *r, const String &fn, const size_t index,
                                                uint8_t *data, const size_t len, const bool final,
                                                const Logger *logger) {
    if (index == 0U) {
        if (logger) {
            logger->logInformation((String("OTA firmware upload start: ") + fn).c_str());
        }
        if (!OtaService::beginFirmware(0, logger)) {
            if (logger) {
                logger->logError("OTA begin firmware failed");
            }
            r->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, OTA_FW_UPLOAD_BEGIN_FAIL_RESP );
            return;
        }
    }
    if (len) {
        if (!OtaService::write(data, len, logger)) {
            if (logger) logger->logError("OTA firmware write failed");
        }
    }
    if (final) {
        const bool ok = OtaService::end(true, logger);
        if (!ok) {
            r->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON,
                    OTA_END_FAIL_RESP);
        } else {
            r->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, OTA_END_FW_UPLOAD_OK);
            xTaskCreatePinnedToCore([](void *) {
                delay(500);
                ESP.restart();
            }, "otaReboot", 2048, nullptr, 1, nullptr, APP_CPU_NUM);
        }
    }
}

void MBXServerHandlers::handleOtaFilesystemUpload(AsyncWebServerRequest *r, const String &fn, const size_t index,
                                                  uint8_t *data, const size_t len, const bool final, const Logger *logger) {
    if (index == 0U) {
        if (logger) logger->logInformation((String("OTA filesystem upload start: ") + fn).c_str());
        if (!OtaService::beginFilesystem(0, logger)) {
            if (logger) {
                logger->logError("OTA begin fs failed");
                r->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, OTA_FS_UPLOAD_BEGIN_FAIL_RESP);
                return;
            }
        }
        if (len) {
            if (!OtaService::write(data, len, logger)) {
                if (logger) logger->logError("OTA fs write failed");
            }
        }
        if (final) {
            const bool ok = OtaService::end(true, logger);
            if (!ok) {
                r->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, OTA_END_FAIL_RESP);
            } else {
                r->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, OTA_END_FS_UPLOAD_OK);
                xTaskCreatePinnedToCore([](void *) {
                    delay(HttpResponseCodes::INTERNAL_SERVER_ERROR);
                    ESP.restart();
                }, "otaReboot", 2048, nullptr, 1, nullptr, APP_CPU_NUM);
            }
        }
    }
}
