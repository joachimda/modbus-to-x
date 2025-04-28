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
MqttLogger mqttLogger([](const char* msg) {
    const auto fullTopic = String(MQTT_ROOT_TOPIC + "/log");
    pubSubClient.publish(fullTopic.c_str(), msg);
});

ModbusManager mb_manager(&commLink, &logger);
AT24CDriver eeprom;

void addHandlers() {
    const auto fullTopic = MQTT_ROOT_TOPIC + MQTT_SUB_NETWORK_RESET;
    subscriptionHandler.addHandler(fullTopic, [](const String &) {
        logger.logInformation("Network reset requested by MQTT message");
        commLink.networkReset();
    });
}

void setup() {
    logger.addTarget(&mqttLogger);
    commLink.begin();
    // mb_manager.initialize();
    // AT24CDriver::begin();
    addHandlers();

}

void loop() {
    logger.logInformation("loop::Loop begin");
    // mb_manager.readRegisters();

    // uint8_t numData = eeprom.readByte(0x0020);

    // commLink.mqttPublish("log", ("Datapoints available: " + String(numData)).c_str());

    // constexpr uint8_t numDataPoints = sizeof(dataPoints) / sizeof(dataPoints[0]);
    // eeprom.writeBuffer(0x0021, (uint8_t *) dataPoints, sizeof(dataPoints));
    //
    // eeprom.writeByte(0x0020, numData + numDataPoints);
    delay(2000);

}
