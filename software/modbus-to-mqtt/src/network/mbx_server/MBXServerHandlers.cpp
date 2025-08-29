#include "network/mbx_server/MBXServerHandlers.h"
#include <atomic>
#include <SPIFFS.h>
#include <WiFi.h>

#include "network/NetworkPortal.h"

namespace {
    std::atomic<NetworkPortal*> g_portal{nullptr};
}

void MBXServerHandlers::setPortal(NetworkPortal* portal) {
    g_portal.store(portal, std::memory_order_release);
}

void MBXServerHandlers::getSsidListAsJson(AsyncWebServerRequest *req) {
    auto* portal = g_portal.load(std::memory_order_acquire);
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
            const auto& ap = (*snap)[i];

            String escSsid;
            escSsid.reserve(ap.SSID.length() + 4);
            for (char c : ap.SSID) {
                if (c == '\\' || c == '\"') { escSsid += '\\'; escSsid += c; }
                else if (static_cast<uint8_t>(c) < 0x20) { escSsid += ' '; }
                else { escSsid += c; }
            }

            char bssidBuf[18] = {0};
            if (ap.BSSID) {
                snprintf(bssidBuf, sizeof(bssidBuf),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         ap.BSSID[0], ap.BSSID[1], ap.BSSID[2],
                         ap.BSSID[3], ap.BSSID[4], ap.BSSID[5]);
            }

            out += R"({"ssid":")"; out += escSsid; out += "\"";
            out += ",\"rssi\":"; out += String(static_cast<long>(ap.RSSI));
            out += ",\"auth\":"; out += String(static_cast<unsigned>(ap.encryptionType));
            out += ",\"channel\":"; out += String(static_cast<unsigned>(ap.channel));
            if (ap.BSSID) { out += R"(,"bssid":")"; out += bssidBuf; out += "\""; }
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

void MBXServerHandlers::handleUpload(AsyncWebServerRequest *r, const String& fn, size_t index, uint8_t *data, size_t len, bool final) {

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

