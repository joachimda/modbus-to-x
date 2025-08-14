#include <Arduino.h>
#include "ArduinoJson.h"
#include <nvs_flash.h>
// #include "MBXServer.h"
#include "commlink/CommLink.h"
#include "MqttLogger.h"
#include "SerialLogger.h"
#include "commlink/MqttSubscriptions.h"
#include "esp_spi_flash.h"
#include "modbus/ModbusManager.h"

Logger logger;
MqttSubscriptionHandler subscriptionHandler(&logger);
WiFiClient wifiClient;
PubSubClient pubSubClient(wifiClient);
CommLink commLink(&subscriptionHandler, &pubSubClient, &logger);
MqttLogger mqttLogger([](const char *msg) {
     const auto logTopic = MQTT_ROOT_TOPIC + PUB_SYSTEM_LOG;
     pubSubClient.publish(logTopic.c_str(), msg);
});
SerialLogger serialLogger(Serial);

ModbusManager modbusManager(&logger);
// MBXServer mbxServer;

void setupEnvironment() {
    logger.useDebug(true);
    Serial.begin(115200);
}

void addSubscriptionHandlers() {
    const auto netReset = MQTT_ROOT_TOPIC + SUB_NETWORK_RESET;
    subscriptionHandler.addHandler(netReset, [](const String &) {
        logger.logInformation("Network reset requested by MQTT message");
        commLink.networkReset();
    });

    const auto mbConfig = MQTT_ROOT_TOPIC + SUB_MODBUS_CONFIG;
    subscriptionHandler.addHandler(mbConfig, [](const String &message) {
        logger.logInformation("New MODBUS config received from MQTT message");
        modbusManager.updateRegisterConfigurationFromJson(message, true);
    });

    const auto echo = MQTT_ROOT_TOPIC + SUB_SYSTEM_ECHO;
    subscriptionHandler.addHandler(echo, [](const String &message) {
        logger.logInformation(message.c_str());
    });

    const auto systemInfo = MQTT_ROOT_TOPIC + SUB_SYSTEM_INFO;
    subscriptionHandler.addHandler(systemInfo, [](const String &) {
        nvs_stats_t stats;
        const esp_err_t err = nvs_get_stats(nullptr, &stats);

        String usedEntries, totalEntries, freeEntries = "";

        if (err == ESP_OK) {
            usedEntries = String(stats.used_entries);
            totalEntries = String(stats.total_entries);
            freeEntries = String(stats.free_entries);
        } else {
            logger.logError(("Failed to get NVS stats: " + String(esp_err_to_name(err))).c_str());
        }
        JsonDocument doc;

        const auto nvsStats = doc["nvsStats"].to<JsonObject>();
        nvsStats["usedEntries"] = usedEntries;
        nvsStats["totalEntries"] = totalEntries;
        nvsStats["freeEntries"] = freeEntries;
        const auto systemStats = doc["systemStats"].to<JsonObject>();
        systemStats["freeHeapSpace"] = ESP.getFreeHeap();
        systemStats["freeSketchSpace"] = ESP.getFreeSketchSpace();
        const auto communication = doc["commlink"].to<JsonObject>();
        communication["mqttBroker"] = commLink.getMqttBroker();
        communication["mqttConnectionState"] = commLink.getMQTTState();
        communication["user"] = commLink.getMQTTUser();
        const auto modbus = doc["modbus"].to<JsonObject>();
        modbus["registerCount"] = modbusManager.getRegisterCount();
        auto userConfig = modbusManager.getUserConfig();
        modbus["communicationMode"] = communicationModesBackwards.at(userConfig.communicationMode);
        modbus["baudRate"] = userConfig.baudRate;

        String out;
        const auto payloadSize = serializeJson(doc, out);
        logger.logDebug(("System Info payload size: " + String(payloadSize)).c_str());
        logger.logInformation(out.c_str());
    });

    const auto registerList = MQTT_ROOT_TOPIC + SUB_MODBUS_CONFIG_LIST;
    subscriptionHandler.addHandler(registerList, [](const String &) {
        const String json = modbusManager.getRegisterConfigurationAsJson();
        logger.logInformation(json.c_str());
    });

    const auto addRegister = MQTT_ROOT_TOPIC + SUB_MODBUS_CONFIG_ADD;
    subscriptionHandler.addHandler(addRegister, [](const String &message) {
        logger.logInformation("Additional MODBUS register configuration received");
        modbusManager.updateRegisterConfigurationFromJson(message, false);
    });
}

void setup() {
    setupEnvironment();
    logger.addTarget(&mqttLogger);
    logger.addTarget(&serialLogger);
    logger.logDebug("setup started");
    logger.logInformation(("Chip Flash size: " + String(ESP.getFlashChipSize())).c_str());
    logger.logInformation(("ESP-IDF Chip Flash size.: " + String(spi_flash_get_chip_size())).c_str());
    logger.logInformation(("Chip Flash Speed.: " + String(ESP.getFlashChipSpeed())).c_str());
    logger.logInformation(("Chip Flash Mode: " + String(ESP.getFlashChipMode())).c_str());

    logger.logInformation(("CPU Freq: " + String(ESP.getCpuFreqMHz())+ "MHz").c_str());
    logger.logInformation(("CPU Cores: " + String(ESP.getChipCores())).c_str());
    logger.logInformation(("Chip Model: " + String(ESP.getChipModel())).c_str());
    logger.logInformation(("Chip Rev.: " + String(ESP.getChipRevision())).c_str());
    logger.logInformation(("Heap Size: " + String(ESP.getHeapSize())).c_str());
    logger.logInformation(("Free Sketch Space: " + String(ESP.getFreeSketchSpace())).c_str());

    addSubscriptionHandlers();
    commLink.begin();
    // modbusManager.initialize();
}

void loop() {
    // mbxServer.begin();
    // mb_manager.readRegisters();
    // commLink.mqttPublish("log", ("Datapoints available: " + String(numData)).c_str());
    delay(500);
}
