#include <Arduino.h>
#include <nvs_flash.h>

#include "ArduinoJson.h"
#include "commlink/CommLink.h"
#include "MqttLogger.h"
#include "commlink/MqttSubscriptions.h"
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

ModbusManager modbusManager(&logger);

void setupEnvironment() {
    if (IS_EMULATED) {
        disableLoopWDT();
        disableCore0WDT();
        disableCore1WDT();
        logger.useDebug(true);
        Serial.begin(115200);
        return;
    }
    if (IS_DEBUG) {
        logger.useDebug(true);
        Serial.begin(115200);
        return;
    }
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
        modbus["registerCount"] = ModbusManager::getRegisterCount();
        modbus["communicationMode"] = modbusManager.getMode();

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
    logger.logDebug("setup started");
    addSubscriptionHandlers();
    commLink.begin();
    modbusManager.initialize();
}

void loop() {
    // mb_manager.readRegisters();
    // commLink.mqttPublish("log", ("Datapoints available: " + String(numData)).c_str());
    delay(500);
}
