#include "network/mbx_server/MBXServerHandlers.h"
#include <atomic>
#include <SPIFFS.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "Config.h"

#include "constants/HttpMediaTypes.h"
#include "constants/HttpResponseCodes.h"
#include "network/NetworkPortal.h"
#include "services/StatService.h"
#include "services/OtaService.h"
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

void MBXServerHandlers::setPortal(NetworkPortal *portal) {
    g_portal.store(portal, std::memory_order_release);
}

void MBXServerHandlers::setMemoryLogger(MemoryLogger *mem) {
    g_memlog.store(mem, std::memory_order_release);
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

/*
 *
* MBXServerHandlers::handleNetworkReset called
E (58724) task_wdt: Task watchdog got triggered. The following tasks did not reset the watchdog in time:
E (58724) task_wdt:  - async_tcp (CPU 0/1)
E (58724) task_wdt: Tasks currently running:
E (58724) task_wdt: CPU 0: wifi
E (58724) task_wdt: CPU 1: IDLE1
E (58724) task_wdt: Aborting.

abort() was called at PC 0x400ff3c0 on core 0
  #0  0x400ff3c0 in task_wdt_isr at /home/runner/work/esp32-arduino-lib-builder/esp32-arduino-lib-builder/esp-idf/components/esp_system/task_wdt.c:158



Backtrace: 0x400837d1:0x3ffbec8c |<-CORRUPTED
  #0  0x400837d1 in panic_abort at /home/runner/work/esp32-arduino-lib-builder/esp32-arduino-lib-builder/esp-idf/components/esp_system/panic.c:408
  #1  0x3ffbec8c in port_IntStack at ??:?


 */

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
        // Hot-reload Modbus configuration
        if (auto *mb = g_mb.load(std::memory_order_acquire)) {
            mb->reconfigureFromFile();
        }
        req->send(HttpResponseCodes::NO_CONTENT);
    }
}

void MBXServerHandlers::handlePutMqttConfigBody(AsyncWebServerRequest *req, const uint8_t *data, const size_t len,
                                                const size_t index, const size_t total) {
    static String body;
    if (index == 0U) body = "";
    body.concat(reinterpret_cast<const char *>(data), len);
    if (index + len < total) return;

    // Write non-sensitive config to SPIFFS
    File f = SPIFFS.open("/conf/mqtt.json", FILE_WRITE);
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
        // Re-enable MQTT now that portal is off, and STA is active
        MqttManager::setMQTTEnabled(true);
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

    const long func = sFunc.toInt();
    const long addr = sAddr.toInt();
    const long len = sLen.toInt();
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
    uint8_t slave = dpDevice ? dpDevice->slaveId : 0;
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
