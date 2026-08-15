#ifndef MODBUS_TO_MQTT_MODBUS_RUNTIME_TYPES_H
#define MODBUS_TO_MQTT_MODBUS_RUNTIME_TYPES_H

#include <Arduino.h>
#include <array>
#include <cstdint>

#include "modbus/config_structs/ModbusDataType.h"
#include "modbus/config_structs/ModbusDatapoint.h"
#include "mqtt/MqttRuntimeTypes.h"

enum class ModbusOperationResult : uint8_t {
    Success = 0,
    QueueFull,
    Timeout,
    Unavailable,
    Shutdown,
    StaleGeneration,
    ActivationFailed,
    Superseded,
};

enum class ModbusPublicationKind : uint8_t {
    Availability = 0,
    Discovery,
};

struct ModbusPublicationCompletion {
    size_t deviceIndex{0U};
    ModbusPublicationKind kind{ModbusPublicationKind::Availability};
    MqttOperationResult result{MqttOperationResult::Unavailable};
    uint32_t modbusGeneration{0U};
    uint32_t connectionEpoch{0U};
    uint32_t attempt{0U};
};

struct ModbusCommandRequest {
    String deviceId;
    String datapointId;
    String sourceTopic;
    uint8_t slaveId{0U};
    bool hasSlaveOverride{false};
    int function{0};
    uint16_t address{0U};
    uint16_t length{0U};
    uint16_t writeValue{0U};
    bool hasWriteValue{false};
    uint32_t expectedGeneration{0U};
};

struct ModbusCommandResult {
    ModbusOperationResult operation{ModbusOperationResult::Unavailable};
    uint8_t busStatus{0xE4U};
    uint8_t slaveId{0U};
    uint16_t count{0U};
    std::array<uint16_t, 16U> words{};
    String rxDump;
    bool hasDatapointMetadata{false};
    ModbusDataType dataType{UINT16};
    RegisterSlice registerSlice{RegisterSlice::Full};
    float scale{1.0F};
    uint32_t generation{0U};
};

struct ModbusRuntimeSnapshot {
    bool available{false};
    bool enabled{false};
    uint32_t generation{0U};
    size_t deviceCount{0U};
    size_t datapointCount{0U};
    uint32_t errorCount{0U};
    uint32_t ownerViolationCount{0U};
    uint32_t commandFailureCount{0U};
};

const char *modbusOperationResultToString(ModbusOperationResult result);

#endif
