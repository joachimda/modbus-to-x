#include "services/StatService.h"

#include <SPIFFS.h>
#include <WiFi.h>

#include "modbus/ModbusManager.h"
#include "network/mbx_server/MBXServerHandlers.h"

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

static String getDeviceName(const String &mac, const String &chipModel, const Logger *logger) {
    String deviceName = WiFiClass::getHostname() ? WiFiClass::getHostname() : "";
    if (deviceName.isEmpty()) {
        String macSuffix = mac.length() >= 8 ? mac.substring(mac.length() - 8) : mac;
        macSuffix.replace(":", "");
        deviceName = String(chipModel ? chipModel : "ESP32") + "-" + macSuffix;
    }

    return deviceName;
}

JsonDocument StatService::appendSystemStats(JsonDocument &document, const Logger *logger) {
    const String mac = WiFi.macAddress();
    const auto chipModel = ESP.getChipModel();

    document["deviceName"] = getDeviceName(mac, chipModel, logger);
    document["fwVersion"] = FW_VERSION;
    document["fwBuildDate"] = String(__DATE__) + " " + String(__TIME__);
    document["buildDate"] = document["fwBuildDate"].as<String>();
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
    MqttManager *link = MBXServerHandlers::getMqttManager();
    const bool connected = (link && link->getMQTTState() == 0);
    document["mqttConnected"] = connected;
    document["broker"] = link ? String(link->getMqttBroker()) : "N/A";
    document["clientId"] = link ? link->getClientId() : "N/A";
    document["lastPublishIso"] = "N/A";
    document["mqttErrorCount"] = 0;
    return document;
}

JsonDocument StatService::appendModbusStats(JsonDocument &document) {
    const ModbusManager *modbusManager = MBXServerHandlers::getModbusManager();

    if (modbusManager == nullptr) {
        return document;
    }

    const auto config = modbusManager->getConfiguration();

    document["buses"] = 1;
    document["devices"] =  config.devices.size();
    size_t totalDatapoints = 0;
    for (const auto &dev : config.devices) {
        totalDatapoints += dev.datapoints.size();
    }

    const bool enabled = ModbusManager::getBusState();

    document["mbusEnabled"] = enabled;
    document["datapoints"] = totalDatapoints;
    document["lastPollIso"] = "";
    document["modbusErrorCount"] = ModbusManager::getBusErrorCount();
    return document;
}

JsonDocument StatService::appendNetworkStats(JsonDocument &document) {
    const bool staConnected = WiFiClass::status() == WL_CONNECTED;
    const wifi_mode_t mode = WiFiClass::getMode();
    const bool apMode = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);

    document["connected"] = staConnected;
    document["apMode"] = apMode;
    document["wifiConnected"] = staConnected;
    document["wifiApMode"] = apMode;

    document["ssid"] = staConnected ? WiFi.SSID() : "";
    document["ip"] = staConnected ? WiFi.localIP().toString() : (apMode ? WiFi.softAPIP().toString() : "");
    document["rssi"] = staConnected ? WiFi.RSSI() : 0;
    document["mac"] = WiFi.macAddress();

    return document;
}
