#include <Arduino.h>
#include <SPIFFS.h>
#include "commlink/CommLink.h"
#include "MqttLogger.h"
#include "MemoryLogger.h"
#include "SerialLogger.h"
#include "commlink/MqttSubscriptions.h"
#include "modbus/ModbusManager.h"
#include "network/mbx_server/MBXServer.h"
#include "Config.h"
#include <network/mbx_server/MBXServerHandlers.h>
#include "services/IndicatorService.h"

Logger logger;
MemoryLogger memoryLogger(300);
MqttSubscriptionHandler subscriptionHandler(&logger);
WiFiClient wifiClient;
PubSubClient pubSubClient(wifiClient);
CommLink commLink(&subscriptionHandler, &pubSubClient, &logger);
SerialLogger serialLogger(Serial);
ModbusManager modbusManager(&logger);
AsyncWebServer server(80);
DNSServer dns;
MBXServer mbxServer(&server, &dns, &logger);

void setupEnvironment() {
    logger.useDebug(true);
    Serial.begin(SERIAL_OUTPUT_BAUD);
}

void setupFs(const Logger * l ) {
    if (!SPIFFS.begin(true)) {
        l->logError("setupFs() - An error occurred while mounting SPIFFS");
        return;
    }
    l->logDebug("setupFs() - SPIFFS mounted");
}

void addSubscriptionHandlers() {
    const auto netReset = MQTT_ROOT_TOPIC + SUB_NETWORK_RESET;
    subscriptionHandler.addHandler(netReset, [](const String &) {
        logger.logInformation("Network reset requested by MQTT message");
    });

}

void setup() {
    setupEnvironment();
    logger.addTarget(&serialLogger);
    logger.addTarget(&memoryLogger);
    logger.logDebug("setup() - logger initialized");
    setupFs(&logger);

    IndicatorService::instance().begin();


    commLink.begin();
    logger.logDebug("setup() - Starting MBX Server");
    MBXServerHandlers::setMemoryLogger(&memoryLogger);
    MBXServerHandlers::setCommLink(&commLink);
    mbxServer.begin();
  

    logger.logDebug("setup() - Starting modbus manager");
    modbusManager.begin();
    logger.logDebug("setup() - complete");
}

void loop() {
    MBXServer::loop();
    // mb_manager.readRegisters();
    // commLink.mqttPublish("log", ("Datapoints available: " + String(numData)).c_str());
    delay(500);
}
