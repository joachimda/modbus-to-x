#include <Arduino.h>
#include <SPIFFS.h>
#include "MemoryLogger.h"
#include "SerialLogger.h"
#include "modbus/ModbusManager.h"
#include "network/mbx_server/MBXServer.h"
#include "Config.h"
#include <network/mbx_server/MBXServerHandlers.h>

#include "mqtt/MqttSubscriptionHandler.h"
#include "services/IndicatorService.h"

Logger logger;
MemoryLogger memory_logger(300);
MqttSubscriptionHandler mqtt_subscription_Handler(&logger);
WiFiClient wifiClient;
PubSubClient pubsub_client(wifiClient);
MqttManager mqtt_manager(&mqtt_subscription_Handler, &pubsub_client, &logger);
SerialLogger serial_logger(Serial);
ModbusManager modbus_manager(&logger);
AsyncWebServer server(80);
DNSServer dns;
MBXServer mbx_server(&server, &dns, &logger);

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

void setup() {
    setupEnvironment();
    logger.addTarget(&serial_logger);
    logger.addTarget(&memory_logger);
    logger.logDebug("setup() - logger initialized");
    setupFs(&logger);

    IndicatorService::instance().begin();

    mqtt_manager.begin();
    logger.logDebug("setup() - Starting MBX Server");
    MBXServerHandlers::setMemoryLogger(&memory_logger);
    MBXServerHandlers::setMqttManager(&mqtt_manager);
    mbx_server.begin();

    logger.logDebug("setup() - Starting modbus manager");
    modbus_manager.begin();
    logger.logDebug("setup() - complete");
}

void loop() {
    MBXServer::loop();
    modbus_manager.loop();
    // mb_manager.readRegisters();
    // mqtt_manager.mqttPublish("log", ("Datapoints available: " + String(numData)).c_str());
    delay(500);
}
