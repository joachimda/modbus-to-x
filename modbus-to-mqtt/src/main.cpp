#include <Arduino.h>
#include <nvs_flash.h>
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

ModbusManager mb_manager(&logger);

void addSubscriptionHandlers() {
    const auto netReset = MQTT_ROOT_TOPIC + SUB_NETWORK_RESET;
    subscriptionHandler.addHandler(netReset, [](const String &) {
        logger.logInformation("Network reset requested by MQTT message");
        commLink.networkReset();
    });

    const auto mbConfig = MQTT_ROOT_TOPIC + SUB_MODBUS_CONFIG;
    subscriptionHandler.addHandler(mbConfig, [](const String &message) {
        logger.logInformation("New MODBUS config received from MQTT message");
        mb_manager.updateRegisterConfigurationFromJson(message, true);
    });

    const auto echo = MQTT_ROOT_TOPIC + SUB_SYSTEM_ECHO;
    subscriptionHandler.addHandler(echo, [](const String &message) {
        logger.logInformation(message.c_str());
    });

    const auto systemInfo = MQTT_ROOT_TOPIC + SUB_SYSTEM_INFO;
    subscriptionHandler.addHandler(systemInfo, [](const String &) {
        nvs_stats_t stats;
        esp_err_t err = nvs_get_stats(nullptr, &stats);  // NULL = current NVS partition

        if (err == ESP_OK) {
            logger.logInformation(("NVS Usage: Used="+String(stats.used_entries) + "  Total=" + String(stats.total_entries) + "  Free=" + String(stats.free_entries)).c_str());
        } else {
            logger.logError(("Failed to get NVS stats: " + String(esp_err_to_name(err))).c_str());
        }
    });

    const auto registerList = MQTT_ROOT_TOPIC + SUB_MODBUS_CONFIG_LIST;
    subscriptionHandler.addHandler(registerList, [](const String &) {
        const String json = mb_manager.getRegisterConfigurationAsJson();
        logger.logInformation(json.c_str());
    });

    const auto addRegister = MQTT_ROOT_TOPIC + SUB_MODBUS_CONFIG_ADD;
    subscriptionHandler.addHandler(addRegister, [](const String &message) {
        logger.logInformation("Additional MODBUS register configuration received");
        mb_manager.updateRegisterConfigurationFromJson(message, true);
    });
}

void setup() {
    disableLoopWDT();
    disableCore0WDT();
    disableCore1WDT();
    Serial.begin(115200);
    logger.useDebug(true);
    logger.addTarget(&mqttLogger);
    logger.logDebug("setup started");
    addSubscriptionHandlers();

    commLink.begin();
    mb_manager.initialize();
}

void loop() {
    // mb_manager.readRegisters();

    // uint8_t numData = eeprom.readByte(0x0020);

    // commLink.mqttPublish("log", ("Datapoints available: " + String(numData)).c_str());

    // constexpr uint8_t numDataPoints = sizeof(dataPoints) / sizeof(dataPoints[0]);
    // eeprom.writeBuffer(0x0021, (uint8_t *) dataPoints, sizeof(dataPoints));
    //
    // eeprom.writeByte(0x0020, numData + numDataPoints);
    delay(100);
}
