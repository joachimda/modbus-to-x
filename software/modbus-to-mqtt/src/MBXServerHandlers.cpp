#include "MBXServerHandlers.h"

static auto jsonEscape(const char* s) -> String {
    String out;
    for (const auto* p = reinterpret_cast<const unsigned char*>(s ? s : ""); *p; ++p) {
        unsigned char c = *p;
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    // \u00XX
                    snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

void MBXServerHandlers::handleNetworkReset() {
    WiFiClass::mode(WIFI_AP_STA);
    WiFi.persistent(true);
    WiFi.disconnect(true, true);
    WiFi.persistent(false);
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
auto MBXServerHandlers::getSsidListAsJson(const std::vector<SSIDRecord>& ssidList) -> String {
    String json = "[";
    bool first = true;
    for (const auto& rec : ssidList) {
        if (!first) {
            json += ",";
        }
        first = false;
        json += R"({"ssid":")";
        json += jsonEscape((rec.ssid.c_str() != nullptr) ? rec.ssid.c_str() : "");
        json += R"(","signal":)";
        json += String(rec.signal);
        json += "}";
    }
    json += "]";
    return json;
}

