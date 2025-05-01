#include <Arduino.h>
#include "commlink/CommLink.h"
#include "AT24CDriver.h"
#include "MqttLogger.h"
#include "commlink/MqttSubscriptions.h"
#include "modbus/ModbusManager.h"

Logger logger;
MqttSubscriptionHandler subscriptionHandler(&logger);
WiFiClient wifiClient;
PubSubClient pubSubClient(wifiClient);
CommLink commLink(&subscriptionHandler, &pubSubClient, &logger);
MqttLogger mqttLogger([](const char *msg) {
    const auto logTopic = MQTT_ROOT_TOPIC + SUB_SYSTEM_LOG;
    pubSubClient.publish(logTopic.c_str(), msg);
});

ModbusManager mb_manager(&logger);
AT24CDriver eeprom;

void addSubscriptionHandlers() {
    const auto netReset = MQTT_ROOT_TOPIC + SUB_NETWORK_RESET;
    subscriptionHandler.addHandler(netReset, [](const String &) {
        logger.logInformation("Network reset requested by MQTT message");
        commLink.networkReset();
    });

    const auto mbConfig = MQTT_ROOT_TOPIC + SUB_MODBUS_CONFIG;
    subscriptionHandler.addHandler(mbConfig, [](const String &message) {
        logger.logInformation("New MODBUS config received from MQTT message");
        mb_manager.updateRegisterConfigurationFromJson(message);
    });

    const auto echo = MQTT_ROOT_TOPIC + SUB_SYSTEM_ECHO;
    subscriptionHandler.addHandler(echo, [](const String &message) {
        logger.logInformation(message.c_str());
    });

    const auto registerList = MQTT_ROOT_TOPIC + SUB_MODBUS_CONFIG_LIST;
    subscriptionHandler.addHandler(MQTT_ROOT_TOPIC + SUB_MODBUS_CONFIG_LIST, [](const String &) {
        const String json = mb_manager.getRegisterConfigurationAsJson();
        logger.logInformation(json.c_str());
    });
}

void setup() {
    logger.useDebug(true);
    logger.addTarget(&mqttLogger);

    logger.logDebug("setup started");
    addSubscriptionHandlers();

    commLink.begin();
    mb_manager.initialize();
    // AT24CDriver::begin();
}

void loop() {
    // mb_manager.readRegisters();

    // uint8_t numData = eeprom.readByte(0x0020);

    // commLink.mqttPublish("log", ("Datapoints available: " + String(numData)).c_str());

    // constexpr uint8_t numDataPoints = sizeof(dataPoints) / sizeof(dataPoints[0]);
    // eeprom.writeBuffer(0x0021, (uint8_t *) dataPoints, sizeof(dataPoints));
    //
    // eeprom.writeByte(0x0020, numData + numDataPoints);
    delay(2000);
}
