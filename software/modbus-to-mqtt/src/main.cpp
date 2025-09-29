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
#include "services/RtcMirrorLogger.h"
#include "services/RtcLogBuffer.h"
#include "services/EspLogBridge.h"
#include <esp_system.h>

Logger logger;
MemoryLogger memory_logger(300);
RtcMirrorLogger rtc_mirror_logger;
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
    // Mirror all app logs to RTC memory so they survive panic resets
    logger.addTarget(&rtc_mirror_logger);
    logger.addTarget(&memory_logger);
    logger.logDebug("setup() - logger initialized");

    // If there are retained logs from a previous crash, prepend them now
    if (RtcLogBuffer::hasData()) {
        memory_logger.logWarning("Recovered logs from previous session (possible crash):");
        RtcLogBuffer::drain([](const char *line, void *user) {
            auto *mem = static_cast<MemoryLogger *>(user);
            mem->logDebug(line);
        }, &memory_logger);
        memory_logger.logWarning("End of recovered logs");
    }

    // Bridge ESP-IDF logs (WiFi/etc.) into MemoryLogger and RTC buffer
    EspLogBridge::begin(&memory_logger);

    // Abnormal reset banner for UI visibility
    switch (esp_reset_reason()) {
        case ESP_RST_UNKNOWN:
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT: {
            const char *reasonStr =
                (esp_reset_reason() == ESP_RST_UNKNOWN) ? "Unknown" :
                (esp_reset_reason() == ESP_RST_PANIC) ? "Panic" :
                (esp_reset_reason() == ESP_RST_INT_WDT) ? "Interrupt WDT" :
                (esp_reset_reason() == ESP_RST_TASK_WDT) ? "Task WDT" :
                (esp_reset_reason() == ESP_RST_WDT) ? "Other WDT" :
                (esp_reset_reason() == ESP_RST_BROWNOUT) ? "Brownout" : "";
            String msg = String("=== Abnormal reset detected: ") + reasonStr + " ===";
            memory_logger.logWarning(msg.c_str());
            break;
        }
        default: break;
    }
    setupFs(&logger);

    IndicatorService::instance().begin();

    mqtt_manager.begin();
    logger.logDebug("setup() - Starting MBX Server");
    MBXServerHandlers::setMemoryLogger(&memory_logger);
    MBXServerHandlers::setMqttManager(&mqtt_manager);
    modbus_manager.setMqttManager(&mqtt_manager);
    MBXServerHandlers::setModbusManager(&modbus_manager);
    mbx_server.begin();

    logger.logDebug("setup() - Starting modbus manager");
    modbus_manager.begin();
    logger.logDebug("setup() - complete");
}

void loop() {
    MBXServer::loop();
    modbus_manager.loop();
    delay(500);
}

