#include "network/mbx_server/MBXServerHandlers.h"
#include <atomic>
#include <array>
#include <memory>
#include "storage/ConfigFs.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <AsyncEventSource.h>
#include <Arduino.h>
#include "Config.h"

#include "constants/HttpMediaTypes.h"
#include "constants/HttpResponseCodes.h"
#include "constants/Routes.h"
#include "network/NetworkPortal.h"
#include "services/StatService.h"
#include "services/OtaService.h"
#include "services/ota/HttpOtaService.h"
#include "services/IndicatorService.h"
#include "modbus/ModbusManager.h"

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
static std::atomic<MqttManager *> g_comm{nullptr};
static std::atomic<ModbusManager *> g_mb{nullptr};
static AsyncEventSource g_events(Routes::EVENTS);
static const Logger *g_eventLogger = nullptr;
static std::atomic<bool> g_eventsAttached{false};
static std::atomic<uint32_t> g_lastPingAt{0};
static std::atomic<size_t> g_lastLogCursor{0};
static std::atomic<uint32_t> g_lastLogCheckAt{0};
static std::atomic<uint32_t> g_eventSeq{0};
static std::atomic<bool> g_otaHttpApplying{false};

constexpr uint32_t STATS_PUSH_INTERVAL_MS = 5000;
constexpr uint32_t STATS_HEARTBEAT_MS = 30000;
constexpr uint32_t STATS_UPTIME_QUANTUM_MS = 10000;
constexpr uint32_t STATS_HEAP_QUANTUM_BYTES = 1024;
constexpr uint32_t LOGS_CHECK_INTERVAL_MS = 1200;
constexpr uint32_t EVENTS_PING_INTERVAL_MS = 30000;
constexpr uint32_t EVENT_RETRY_MS = 5000;
constexpr size_t LOG_CHUNK_BYTES = 2048;

enum class StatsCategory : uint8_t {
    System = 0,
    Network,
    MQTT,
    Modbus,
    Storage,
    Health,
    Count
};

constexpr const char *STAT_EVENT_NAMES[] = {
    "stats-system",
    "stats-network",
    "stats-mqtt",
    "stats-modbus",
    "stats-storage",
    "stats-health"
};

static_assert(static_cast<size_t>(StatsCategory::Count) == (sizeof(STAT_EVENT_NAMES) / sizeof(STAT_EVENT_NAMES[0])),
              "STAT_EVENT_NAMES size mismatch");

static std::array<String, static_cast<size_t>(StatsCategory::Count)> g_statsPayload;
static std::array<uint32_t, static_cast<size_t>(StatsCategory::Count)> g_statsLastSent{};

bool parseConnectPayload(uint8_t *data, size_t len, String &ssid, String &pass,
                         String &bssid, bool &save, WifiStaticConfig &st,
                         uint8_t &channel) {
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

namespace {

bool eventStreamReady() {
    return g_eventsAttached.load(std::memory_order_acquire);
}

bool eventStreamHasClients() {
    return eventStreamReady() && g_events.count() > 0;
}

uint32_t nextEventId() {
    return g_eventSeq.fetch_add(1, std::memory_order_relaxed) + 1;
}

size_t statsIndex(const StatsCategory c) {
    return static_cast<size_t>(c);
}

uint32_t roundDown(const uint32_t value, const uint32_t quantum) {
    if (quantum == 0) return value;
    return (value / quantum) * quantum;
}

String buildStatsPayload(const StatsCategory cat) {
    JsonDocument doc;
    switch (cat) {
        case StatsCategory::System: {
            doc = StatService::appendSystemStats(doc, g_eventLogger);
            const uint32_t uptime = doc["uptimeMs"] | 0;
            doc["uptimeMs"] = roundDown(uptime, STATS_UPTIME_QUANTUM_MS);
            doc["heapFree"] = roundDown(static_cast<uint32_t>(doc["heapFree"] | 0), STATS_HEAP_QUANTUM_BYTES);
            doc["heapMin"] = roundDown(static_cast<uint32_t>(doc["heapMin"] | 0), STATS_HEAP_QUANTUM_BYTES);
            break;
        }
        case StatsCategory::Network: {
            doc = StatService::appendNetworkStats(doc);
            break;
        }
        case StatsCategory::MQTT: {
            doc = StatService::appendMQTTStats(doc);
            break;
        }
        case StatsCategory::Modbus: {
            doc = StatService::appendModbusStats(doc);
            break;
        }
        case StatsCategory::Storage: {
            doc = StatService::appendStorageStats(doc);
            break;
        }
        case StatsCategory::Health: {
            doc = StatService::appendHealthStats(doc);
            break;
        }
        case StatsCategory::Count:
        default: break;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

void sendStatsPayload(const StatsCategory cat, const String &payload, const uint32_t now, AsyncEventSourceClient *client) {
    const auto idx = statsIndex(cat);
    g_statsPayload[idx] = payload;
    g_statsLastSent[idx] = now;
    if (client) {
        client->send(payload.c_str(), STAT_EVENT_NAMES[idx], nextEventId());
    } else {
        g_events.send(payload.c_str(), STAT_EVENT_NAMES[idx], nextEventId());
    }
}

void emitStats(bool force, AsyncEventSourceClient *client) {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < static_cast<uint8_t>(StatsCategory::Count); ++i) {
        const auto cat = static_cast<StatsCategory>(i);
        const String payload = buildStatsPayload(cat);
        if (payload.isEmpty()) continue;

        const auto idx = statsIndex(cat);
        const bool changed = payload != g_statsPayload[idx];
        const uint32_t last = g_statsLastSent[idx];
        const bool heartbeat = (now - last) >= STATS_HEARTBEAT_MS;

        if (force || changed || heartbeat) {
            sendStatsPayload(cat, payload, now, client);
        }
    }
}

String readLogChunk(MemoryLogger *mem, size_t start, size_t len) {
    String out;
    if (!mem || len == 0) {
        return out;
    }
    std::unique_ptr<char[]> buf(new char[len + 1]);
    const size_t wrote = mem->copyAsText(start, reinterpret_cast<uint8_t *>(buf.get()), len);
    buf[wrote] = '\0';
    out = buf.get();
    return out;
}

void sendLogPayload(const String &text, const bool truncated, const char *eventName) {
    if (text.isEmpty()) {
        return;
    }
    JsonDocument doc;
    doc["text"] = text;
    doc["truncated"] = truncated;
    String payload;
    serializeJson(doc, payload);
    g_events.send(payload.c_str(), eventName, nextEventId());
}

void sendInitialLogsToClient(AsyncEventSourceClient *client) {
    auto *mem = g_memlog.load(std::memory_order_acquire);
    if (!client || !mem) {
        return;
    }

    const size_t total = mem->flattenedSize();
    if (total == 0) {
        return;
    }
    const size_t start = (total > LOG_CHUNK_BYTES) ? (total - LOG_CHUNK_BYTES) : 0;
    const String text = readLogChunk(mem, start, total - start);
    if (text.isEmpty()) {
        return;
    }

    JsonDocument doc;
    doc["text"] = text;
    doc["truncated"] = start > 0;
    String payload;
    serializeJson(doc, payload);
    client->send(payload.c_str(), "logs", nextEventId());
}

void broadcastLogDelta() {
    auto *mem = g_memlog.load(std::memory_order_acquire);
    if (!mem) {
        return;
    }

    const size_t total = mem->flattenedSize();
    if (total == 0) {
        g_lastLogCursor.store(0, std::memory_order_relaxed);
        return;
    }

    size_t lastCursor = g_lastLogCursor.load(std::memory_order_relaxed);
    if (lastCursor > total) {
        // Buffer rolled; send the latest window
        const size_t start = (total > LOG_CHUNK_BYTES) ? (total - LOG_CHUNK_BYTES) : 0;
        const String text = readLogChunk(mem, start, total - start);
        sendLogPayload(text, true, "logs");
        g_lastLogCursor.store(total, std::memory_order_relaxed);
        return;
    }

    size_t remaining = total - lastCursor;
    if (remaining == 0) {
        return;
    }

    size_t offset = lastCursor;
    while (remaining > 0) {
        const size_t chunk = remaining > LOG_CHUNK_BYTES ? LOG_CHUNK_BYTES : remaining;
        const String text = readLogChunk(mem, offset, chunk);
        sendLogPayload(text, false, "log");
        offset += chunk;
        remaining -= chunk;
    }
    g_lastLogCursor.store(total, std::memory_order_relaxed);
}

} // namespace

void MBXServerHandlers::setPortal(NetworkPortal *portal) {
    g_portal.store(portal, std::memory_order_release);
}

void MBXServerHandlers::setMemoryLogger(MemoryLogger *mem) {
    g_memlog.store(mem, std::memory_order_release);
}

void MBXServerHandlers::initEventStream(AsyncWebServer *server, const Logger *logger) {
    g_eventLogger = logger;
    bool expected = false;
    if (!g_eventsAttached.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    g_events.onConnect([](AsyncEventSourceClient *client) {
        if (!client) {
            return;
        }
        client->send("ready", "ping", nextEventId(), EVENT_RETRY_MS);

        // Warm-up snapshot for new client
        emitStats(true, client);
        sendInitialLogsToClient(client);
    });

    server->addHandler(&g_events);
}

void MBXServerHandlers::pumpEventStream() {
    if (!eventStreamHasClients()) {
        return;
    }

    const uint32_t now = millis();
    static uint32_t lastStatsPoll = 0;
    if (now - lastStatsPoll >= STATS_PUSH_INTERVAL_MS) {
        lastStatsPoll = now;
        emitStats(false, nullptr);
    }

    if (now - g_lastLogCheckAt.load(std::memory_order_relaxed) >= LOGS_CHECK_INTERVAL_MS) {
        g_lastLogCheckAt.store(now, std::memory_order_relaxed);
        broadcastLogDelta();
    }

    if (now - g_lastPingAt.load(std::memory_order_relaxed) >= EVENTS_PING_INTERVAL_MS) {
        g_lastPingAt.store(now, std::memory_order_relaxed);
        g_events.send("ping", "ping", nextEventId());
    }
}

void MBXServerHandlers::handleCaptivePortalRedirect(AsyncWebServerRequest *req) {
    const IPAddress apIp = WiFi.softAPIP();
    const String target = String("http://") + apIp.toString() + Routes::ROOT;
    String html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
    html += R"(<meta http-equiv="refresh" content="0; url=)" + target + "\">";
    html += "<title>Configuration Portal</title></head><body>";
    html += "<p>Redirecting to <a href=\"" + target + "\">configuration portal</a>...</p>";
    html += "</body></html>";

    auto *response = req->beginResponse(HttpResponseCodes::OK, HttpMediaTypes::HTML, html);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Location", target);
    req->send(response);
}

void MBXServerHandlers::setMqttManager(MqttManager *mqttManager) {
    g_comm.store(mqttManager, std::memory_order_release);
}

MqttManager *MBXServerHandlers::getMqttManager() {
    return g_comm.load(std::memory_order_acquire);
}

void MBXServerHandlers::setModbusManager(ModbusManager *modbusManager) {
    g_mb.store(modbusManager, std::memory_order_release);
}

ModbusManager *MBXServerHandlers::getModbusManager() {
    return g_mb.load(std::memory_order_acquire);
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
    xTaskCreatePinnedToCore([](void *) {
        WiFi.persistent(true);
        WiFi.setAutoReconnect(false);
        esp_wifi_set_storage(WIFI_STORAGE_FLASH);

        wifi_config_t emptyStaConfig{};
        const esp_err_t cfgRes = esp_wifi_set_config(WIFI_IF_STA, &emptyStaConfig);
        Serial.printf("handleNetworkReset: esp_wifi_set_config(WIFI_IF_STA) -> %d\n", static_cast<int>(cfgRes));
        const esp_err_t restoreRes = esp_wifi_restore();
        Serial.printf("handleNetworkReset: esp_wifi_restore() -> %d\n", static_cast<int>(restoreRes));
        WiFi.disconnect(true, true);
        const bool eraseOk = WiFi.eraseAP(); // clear credentials stored in NVS
        WiFi.eraseAP();
        Serial.printf("handleNetworkReset: WiFi.eraseAP() -> %s\n", eraseOk ? "true" : "false");

        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        WiFi.persistent(false);
        WiFiClass::mode(WIFI_MODE_APSTA);
        vTaskDelay(pdMS_TO_TICKS(200)); // let flash writes finish
        ESP.restart();
    }, "netReset", 4096, nullptr, 1, nullptr, APP_CPU_NUM);
}

void MBXServerHandlers::handlePutModbusConfigBody(AsyncWebServerRequest *req, const uint8_t *data, const size_t len,
                                                  const size_t index,
                                                  const size_t total) {
    static File bodyFile;
    if (index == 0U) {
        bodyFile = ConfigFS.open(ConfigFs::kModbusConfigFile, FILE_WRITE);
    }
    if (bodyFile) {
        bodyFile.write(data, len);
    }
    if (index + len == total) {
        if (bodyFile) bodyFile.close();
        // Hot-reload Modbus configuration
        if (auto *mb = g_mb.load(std::memory_order_acquire)) {
            mb->reconfigureFromFile();
        }
        req->send(HttpResponseCodes::NO_CONTENT);
    }
}

void MBXServerHandlers::handleModbusDisable(AsyncWebServerRequest *req, bool state) {
        if (const auto *mb = g_mb.load(std::memory_order_acquire)) {
            ModbusManager::setModbusEnabled(state);
        }
        req->send(HttpResponseCodes::OK);
}

void MBXServerHandlers::handlePutMqttConfigBody(AsyncWebServerRequest *req, const uint8_t *data, const size_t len,
                                                const size_t index, const size_t total) {
    static String body;
    if (index == 0U) body = "";
    body.concat(reinterpret_cast<const char *>(data), len);
    if (index + len < total) return;

    // Write non-sensitive config to config FS
    File f = ConfigFS.open(ConfigFs::kMqttConfigFile, FILE_WRITE);
    if (!f) {
        req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        return;
    }
    f.print(body);
    f.close();

    // Hot-reload MQTT configuration
    if (auto *link = g_comm.load(std::memory_order_acquire)) {
        link->reconfigureFromFile();
    }

    req->send(HttpResponseCodes::NO_CONTENT);
}

void MBXServerHandlers::handlePutMqttSecretBody(AsyncWebServerRequest *req, const uint8_t *data, const size_t len,
                                                const size_t index, const size_t total) {
    static String body;
    if (index == 0U) body = "";
    body.concat(reinterpret_cast<const char *>(data), len);
    if (index + len < total) return;

    JsonDocument doc;
    const DeserializationError derr = deserializeJson(doc, body);
    if (derr) {
        req->send(HttpResponseCodes::BAD_REQUEST, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        return;
    }
    const String pass = doc["password"] | "";
    Preferences prefs;
    prefs.begin(MQTT_PREFS_NAMESPACE, false);
    prefs.putString("pass", pass);
    prefs.end();
    req->send(HttpResponseCodes::NO_CONTENT);
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
        // Re-evaluate MQTT preference now that portal is off, and STA is active
        if (auto *link = getMqttManager()) {
            link->reconfigureFromFile();
        }
        vTaskDelete(nullptr);
    }, "apOff", 2048, nullptr, 1, nullptr, APP_CPU_NUM);
}

void MBXServerHandlers::getSystemStats(AsyncWebServerRequest *req, const Logger *logger) {
    logger->logDebug(
        ("MBX Server: Started processing "
         + String(req->methodToString()) + " request on " + req->url()).c_str());

    JsonDocument doc;
    doc = StatService::appendSystemStats(doc, logger);
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
        constexpr size_t MAX_LOG_BYTES = 8192;
        const size_t totalSize = mem->flattenedSize();
        const size_t offset = (totalSize > MAX_LOG_BYTES) ? (totalSize - MAX_LOG_BYTES) : 0;
        const size_t payloadSize = totalSize - offset;

        auto filler = [mem, offset, payloadSize](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= payloadSize || maxLen == 0) {
                return 0;
            }
            const size_t remaining = payloadSize - index;
            const size_t chunk = (remaining < maxLen) ? remaining : maxLen;
            return mem->copyAsText(offset + index, buffer, chunk);
        };

        auto *response = req->beginResponse("text/plain; charset=utf-8", payloadSize, filler);
        response->addHeader("Cache-Control", "no-store");
        response->addHeader("X-Log-Truncated", offset > 0 ? "true" : "false");
        req->send(response);
    } else {
        req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::PLAIN_TEXT, "logging buffer unavailable");
    }
}

void MBXServerHandlers::handleMqttTestConnection(AsyncWebServerRequest *req) {
    auto *link = MBXServerHandlers::getMqttManager();
    JsonDocument doc;
    if (!link) {
        doc["ok"] = false;
        doc["error"] = "mqtt_unavailable";
        String out;
        serializeJson(doc, out);
        req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::JSON, out);
        return;
    }

    const bool ok = link->testConnectOnce();
    doc["ok"] = ok;
    doc["broker"] = link->getMqttBroker();
    doc["user"] = link->getMQTTUser();
    doc["state"] = link->getMQTTState();
    String out;
    serializeJson(doc, out);
    req->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, out);
}

void MBXServerHandlers::handleModbusExecute(AsyncWebServerRequest *req) {
    auto getParam = [&](const char *name, String &out) -> bool {
        if (!req->hasParam(name)) return false;
        out = req->getParam(name)->value();
        return out.length() > 0;
    };

    String devId, dpId, sFunc, sAddr, sLen;
    if (!getParam("devId", devId) || !getParam("dpId", dpId)
        || !getParam("func_code", sFunc) || !getParam("addr", sAddr)
        || !getParam("len", sLen)) {
        req->send(HttpResponseCodes::BAD_REQUEST, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        return;
    }
    String sSlave;
    const bool hasSlaveOverride = getParam("slave", sSlave);

    const long func = sFunc.toInt();
    const long addr = sAddr.toInt();
    long len = sLen.toInt();
    const long slaveOverride = hasSlaveOverride ? sSlave.toInt() : 0;
    const bool slaveOverrideValid = slaveOverride > 0 && slaveOverride <= 247;
    if (func <= 0 || addr < 0 || len <= 0) {
        req->send(HttpResponseCodes::BAD_REQUEST, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        return;
    }

    // Optional value for write operations (ignored here but echoed back)
    String sValue;
    if (req->hasParam("value")) sValue = req->getParam("value")->value();

    // Execute against Modbus
    JsonDocument doc;
    ModbusManager *mb = getModbusManager();
    if (!mb) {
        doc["ok"] = false;
        doc["error"] = "modbus_unavailable";
        sendJson(req, doc);
        return;
    }

    // Resolve slave id by datapoint
    const ModbusDevice *dpDevice = nullptr;
    const ModbusDatapoint *dpMeta = mb->findDatapointById(dpId, &dpDevice);
    uint8_t slave = 0;
    if (slaveOverrideValid) {
        slave = static_cast<uint8_t>(slaveOverride);
    } else if (dpDevice) {
        slave = dpDevice->slaveId;
    } else {
        const ConfigurationRoot &cfg = mb->getConfiguration();
        for (const auto &dev: cfg.devices) {
            if (dev.id == devId) {
                slave = dev.slaveId;
                break;
            }
        }
    }
    if (slave == 0) slave = MODBUS_SLAVE_ID;

    uint16_t outBuf[16]{};
    uint16_t outCount = 0;
    String rxDump;
    uint16_t writeVal = 0;
    bool hasWriteVal = false;
    if (sValue.length()) {
        if (sValue.equalsIgnoreCase("true") || sValue == "1") writeVal = 1;
        else if (sValue.equalsIgnoreCase("false") || sValue == "0") writeVal = 0;
        else writeVal = static_cast<uint16_t>(sValue.toInt());
        hasWriteVal = true;
    }

    if (func == 5 || func == 6 || func == 16) {
        len = 1; // single write workaround even for FC16
    }

    const uint8_t status = mb->executeCommand(slave, (int) func, (uint16_t) addr, (uint16_t) len,
                                              writeVal, hasWriteVal,
                                              outBuf, 16, outCount, rxDump);

    doc["ok"] = (status == 0);
    doc["code"] = status;
    doc["state"] = ModbusManager::statusToString(status);
    doc["devId"] = devId;
    doc["dpId"] = dpId;
    doc["request"]["func_code"] = func;
    doc["request"]["addr"] = addr;
    doc["request"]["len"] = len;
    if (sValue.length()) doc["request"]["value"] = sValue;
    if (rxDump.length()) doc["rx_dump"] = rxDump;
    if (outCount > 0) {
        JsonArray raw = doc["result"]["raw"].to<JsonArray>();
        for (uint16_t i = 0; i < outCount; ++i) {
            (void) raw.add(outBuf[i]);
        }
        if (dpMeta && dpMeta->dataType == TEXT) {
            doc["result"]["value"] = ModbusManager::registersToAscii(outBuf, outCount);
        } else if (dpMeta) {
            const uint16_t rawWord = outCount > 0 ? outBuf[0] : 0;
            const uint16_t sliced = ModbusManager::sliceRegister(rawWord, dpMeta->registerSlice);
            const float value = static_cast<float>(sliced) * dpMeta->scale;
            doc["result"]["value"] = value;
        } else {
            doc["result"]["value"] = outBuf[0];
        }
    }

    sendJson(req, doc);
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
            r->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, OTA_FW_UPLOAD_BEGIN_FAIL_RESP);
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
                                                  uint8_t *data, const size_t len, const bool final,
                                                  const Logger *logger) {
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

void MBXServerHandlers::handleOtaHttpCheck(AsyncWebServerRequest *req, const Logger *logger) {
    (void)logger;
#if OTA_HTTP_ENABLED
    bool refresh = true;
    if (req->hasParam("refresh")) {
        const String value = req->getParam("refresh")->value();
        refresh = !(value == "0" || value.equalsIgnoreCase("false"));
    }
    if (refresh) {
        HttpOtaService::checkNow();
    }

    bool ok = false;
    bool available = false;
    String version;
    String error;
    HttpOtaService::getLastCheckStatus(ok, available, version, error);
    const bool pending = HttpOtaService::isCheckPending();

    JsonDocument doc;
    doc["ok"] = ok;
    doc["available"] = available;
    doc["pending"] = pending;
    if (available && version.length()) doc["version"] = version;
    if (!pending && !ok && error.length()) doc["error"] = error;
    sendJson(req, doc);
#else
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "ota_http_disabled";
    sendJson(req, doc);
#endif
}

void MBXServerHandlers::handleOtaHttpNotes(AsyncWebServerRequest *req, const Logger *logger) {
    (void)logger;
#if OTA_HTTP_ENABLED
    String version;
    if (!HttpOtaService::hasPendingUpdate(version)) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["error"] = "no_update";
        sendJson(req, doc);
        return;
    }

    bool refresh = true;
    if (req->hasParam("refresh")) {
        const String value = req->getParam("refresh")->value();
        refresh = !(value == "0" || value.equalsIgnoreCase("false"));
    }
    if (refresh) {
        HttpOtaService::requestReleaseNotes();
    }

    bool ready = false;
    bool pending = false;
    String notes;
    String error;
    HttpOtaService::getNotesStatus(ready, pending, notes, error);

    JsonDocument doc;
    doc["ok"] = ready || pending;
    doc["pending"] = pending;
    doc["available"] = true;
    if (version.length()) doc["version"] = version;
    if (ready) {
        doc["notes"] = notes;
    } else if (!pending && error.length()) {
        doc["ok"] = false;
        doc["error"] = error;
    }
    sendJson(req, doc);
#else
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "ota_http_disabled";
    sendJson(req, doc);
#endif
}

void MBXServerHandlers::handleOtaHttpApply(AsyncWebServerRequest *req, const Logger *logger) {
    (void)logger;
#if OTA_HTTP_ENABLED
    JsonDocument doc;
    String version;
    if (!HttpOtaService::hasPendingUpdate(version)) {
        doc["ok"] = false;
        doc["error"] = "no_update";
        sendJson(req, doc);
        return;
    }

    if (g_otaHttpApplying.exchange(true)) {
        doc["ok"] = false;
        doc["error"] = "ota_in_progress";
        sendJson(req, doc);
        return;
    }

    doc["ok"] = true;
    doc["started"] = true;
    if (version.length()) doc["version"] = version;
    sendJson(req, doc);

    auto *loggerPtr = const_cast<Logger *>(logger);
    xTaskCreatePinnedToCore([](void *param) {
        auto *log = static_cast<Logger *>(param);
        if (log) log->logInformation("HTTP-OTA: Apply started");
        String error;
        const bool ok = HttpOtaService::applyPendingUpdate(error);
        if (!ok) {
            const String msg = "HTTP-OTA: Apply failed: " + error;
            if (log) log->logError(msg.c_str());
            Serial.println(msg);
            g_otaHttpApplying.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }
        if (log) log->logInformation("HTTP-OTA: Apply complete, rebooting");
        Serial.println("HTTP-OTA: Apply complete, rebooting");
        g_otaHttpApplying.store(false, std::memory_order_release);
        delay(500);
        ESP.restart();
    }, "otaHttpApply", 8192, loggerPtr, 1, nullptr, APP_CPU_NUM);
#else
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "ota_http_disabled";
    sendJson(req, doc);
#endif
}
