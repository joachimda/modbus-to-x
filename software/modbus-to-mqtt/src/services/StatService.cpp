#include "services/StatService.h"

#include <SPIFFS.h>
#include <WiFi.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static const char *resetReasonToString(const esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "Power on";
        case ESP_RST_EXT: return "External (reset pin)";
        case ESP_RST_SW: return "Software reset";
        case ESP_RST_PANIC: return "Panic";
        case ESP_RST_INT_WDT: return "Interrupt WDT";
        case ESP_RST_TASK_WDT: return "Task WDT";
        case ESP_RST_WDT: return "Other WDT";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
        case ESP_RST_BROWNOUT: return "Brownout";
        case ESP_RST_SDIO: return "SDIO";
        default: return "Unknown";
    }
}

static String getDeviceName(const String &mac, const String &chipModel) {
    String deviceName = WiFi.getHostname() ? WiFi.getHostname() : "";
    if (deviceName.isEmpty()) {
        String macSuffix = mac.length() >= 8 ? mac.substring(mac.length() - 8) : mac;
        macSuffix.replace(":", "");
        deviceName = String(chipModel ? chipModel : "ESP32") + "-" + macSuffix;
    }

    return deviceName;
}

JsonDocument StatService::appendSystemStats(JsonDocument &document) {
    const String mac = WiFi.macAddress();
    const auto chipModel = ESP.getChipModel();

    document["deviceName"] = getDeviceName(mac, chipModel);
    document["fwVersion"] = FW_VERSION;
    document["fwBuildDate"] = String(__DATE__) + " " + String(__TIME__);
    document["chipModel"] = chipModel;
    document["chipRevision"] = ESP.getChipRevision();
    document["cpuFreqMHz"] = ESP.getCpuFreqMHz();
    document["sdkVersion"] = ESP.getSdkVersion();
    document["uptimeMs"] = static_cast<uint32_t>(millis());
    document["heapFree"] = ESP.getFreeHeap();
    document["heapMin"] = ESP.getMinFreeHeap();
    document["resetReason"] = resetReasonToString(esp_reset_reason());
    return document;
}

JsonDocument StatService::appendStorageStats(JsonDocument &document) {
    document["flashSize"] = ESP.getFlashChipSize();
    document["spiffsTotal"] = SPIFFS.totalBytes();
    document["spiffsUsed"] = SPIFFS.usedBytes();
    return document;
}

JsonDocument StatService::appendHealthStats(JsonDocument &document) {
    document["ok"] = true;
    const auto comps = document["components"].to<JsonObject>();
    comps["wifi"] = "ok";
    comps["mqtt"] = "ok";
    comps["modbus"] = "ok";
    comps["fs"] = "ok";
    return document;
}

JsonDocument StatService::appendMQTTStats(JsonDocument &document) {
    document["connected"] = false;
    document["broker"] = "N/A";
    document["clientId"] = "N/A";
    document["lastPublishIso"] = "";
    document["errorCount"] = 0;
    return document;
}

JsonDocument StatService::appendModbusStats(JsonDocument &document) {
    document["buses"] = 1;
    document["devices"] = 0;
    document["datapoints"] = 0;
    document["pollIntervalMs"] = 0;
    document["lastPollIso"] = "";
    document["errorCount"] = 0;
    return document;
}

JsonDocument StatService::appendNetworkStats(JsonDocument &document) {
    const bool staConnected = WiFiClass::status() == WL_CONNECTED;
    const wifi_mode_t mode = WiFiClass::getMode();
    const bool apMode = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);

    document["connected"] = staConnected;
    document["apMode"] = apMode;

    document["ssid"] = staConnected ? WiFi.SSID() : "";
    document["ip"] = staConnected ? WiFi.localIP().toString() : (apMode ? WiFi.softAPIP().toString() : "");
    document["rssi"] = staConnected ? WiFi.RSSI() : 0;
    document["mac"] = WiFi.macAddress();

    return document;
}
