#ifndef MODBUS_MQTT_BRIDGE_H
#define MODBUS_MQTT_BRIDGE_H

#include <Arduino.h>
#include <vector>

#include "modbus/config_structs/ConfigurationRoot.h"
#include "modbus/config_structs/ModbusDatapoint.h"
#include "modbus/config_structs/ModbusDevice.h"

class Logger;
class MqttManager;
class ModbusManager;

class ModbusMqttBridge {
public:
    ModbusMqttBridge(Logger *logger, ModbusManager *modbus);

    void setMqttManager(MqttManager *mqtt);

    void onConfigurationLoaded(ConfigurationRoot &root);

    void onConnectionState(bool connectedNow, bool connectedLast, ConfigurationRoot &root);

    void publishDatapoint(ModbusDevice &device, const ModbusDatapoint &dp, const String &payload) const;

private:
    void handleMqttConnected(ConfigurationRoot &root);

    static void handleMqttDisconnected(ConfigurationRoot &root);

    void rebuildWriteSubscriptions(const ConfigurationRoot &root);

    void handleWriteCommand(const String &topic,
                            uint8_t slaveId,
                            ModbusFunctionType fn,
                            uint16_t addr,
                            uint8_t numRegs,
                            float scale,
                            const String &payload) const;

    String buildDatapointTopic(const ModbusDevice &device, const ModbusDatapoint &dp) const;
    String buildAvailabilityTopic(const ModbusDevice &device) const;

    void publishAvailabilityOnline(ModbusDevice &device) const;
    void publishHomeAssistantDiscovery(ModbusDevice &device) const;

    Logger *_logger;
    ModbusManager *_modbus;
    MqttManager *_mqtt{nullptr};
    std::vector<String> _writeTopics;
};

#endif
