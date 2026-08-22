#ifndef MODBUS_MQTT_BRIDGE_H
#define MODBUS_MQTT_BRIDGE_H

#include <Arduino.h>
#include <atomic>

#include "modbus/config_structs/ConfigurationRoot.h"
#include "modbus/ModbusRuntimeTypes.h"
#include "mqtt/MqttRuntimeTypes.h"

class Logger;
class MqttManager;
class ModbusManager;

class ModbusMqttBridge {
public:
    ModbusMqttBridge(Logger *logger, ModbusManager *modbus);

    void setMqttManager(MqttManager *mqtt);

    MqttOperationResult stageConfiguration(const ConfigurationRoot &root, uint32_t modbusGeneration);

    void commitConfiguration(uint32_t modbusGeneration);

    void onConfigurationActivated(ConfigurationRoot &root);

    void onConnectionState(const MqttStatusSnapshot &mqttStatus, ConfigurationRoot &root,
                           uint32_t modbusGeneration);

    void publishDatapoint(ModbusDevice &device, const ModbusDatapoint &datapoint,
                          const String &payload, uint32_t modbusGeneration) const;

    void onPublicationCompletion(ConfigurationRoot &root,
                                 const ModbusPublicationCompletion &completion,
                                 uint32_t modbusGeneration);

    void recoverLostPublicationCompletions(ConfigurationRoot &root);

private:
    MqttBridgePlan buildBridgePlan(const ConfigurationRoot &root, uint32_t modbusGeneration) const;

    void handleMqttConnected(ConfigurationRoot &root, uint32_t modbusGeneration);

    static void handleMqttDisconnected(ConfigurationRoot &root);

    String buildDatapointTopic(const ModbusDevice &device, const ModbusDatapoint &datapoint,
                               const String &rootTopic) const;

    String buildAvailabilityTopic(const ModbusDevice &device, const String &rootTopic) const;

    void publishAvailabilityOnline(ModbusDevice &device, size_t deviceIndex,
                                   const MqttStatusSnapshot &mqttStatus,
                                   uint32_t modbusGeneration) const;

    void publishHomeAssistantDiscovery(ModbusDevice &device, size_t deviceIndex,
                                       const MqttStatusSnapshot &mqttStatus,
                                       uint32_t modbusGeneration) const;

    MqttPublishCallback buildPublishCallback(size_t deviceIndex,
                                             ModbusPublicationKind kind,
                                             uint32_t modbusGeneration,
                                             uint32_t connectionEpoch,
                                             uint32_t attempt) const;

    Logger *_logger;
    ModbusManager *_modbus;
    MqttManager *_mqtt{nullptr};
    uint32_t _lastConnectionEpoch{0U};
    mutable std::atomic<bool> _publicationCompletionLost{false};
};

#endif
