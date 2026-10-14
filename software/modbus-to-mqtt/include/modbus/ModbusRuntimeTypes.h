#ifndef MODBUS_TO_MQTT_MODBUS_RUNTIME_TYPES_H
#define MODBUS_TO_MQTT_MODBUS_RUNTIME_TYPES_H

#include <Arduino.h>
#include <array>
#include <cstdint>

#include "modbus/ModbusMqttWriteCore.h"
#include "modbus/ModbusValueCodec.h"
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
    OtaInProgress,
};

enum class ModbusOtaQuiesceResult : uint8_t {
    None = 0,
    Stopped,
    AlreadyDisabled,
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
    // Wavin functions (0x43/0x44): address is the register index within
    // category/page. Without hasWavinAddress they come from the saved
    // datapoint the request names.
    uint8_t category{0U};
    uint8_t page{0U};
    bool hasWavinAddress{false};
    // FC5: coil state, non-zero switches the coil on (MQTT command path).
    uint16_t writeValue{0U};
    // FC6/FC16/0x44: register words already encoded for the datapoint's data type
    // and word order (MQTT command path).
    std::array<uint16_t, ModbusValueCodec::kMaxWords> writeWords{};
    uint8_t writeCount{0U};
    // HTTP execute path: the value as text, converted by the owner with the
    // MQTT command grammar. FC5: 0/1/false/true. FC6/FC16 (writeCount 0):
    // the engineering value, encoded with the saved datapoint's data type,
    // scale and word order.
    String writeText;
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
    ModbusWordOrder wordOrder{ModbusWordOrder::Big};
    float scale{1.0F};
    // Set when a write value could not be converted; busStatus is then
    // ku8MBIllegalDataValue and the bus was not touched.
    ModbusMqttWriteCore::RejectionReason writeRejection{ModbusMqttWriteCore::RejectionReason::None};
    // Set when a register write named a datapoint the active configuration
    // does not contain, so no data type, scale or word order was available
    // to encode the value; the bus was not touched.
    bool unknownDatapoint{false};
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
    size_t commandQueueDepth{0U};
    bool otaQuiesced{false};
    uint32_t otaOperationId{0U};
    ModbusOtaQuiesceResult otaQuiesceResult{ModbusOtaQuiesceResult::None};
};

const char *modbusOperationResultToString(ModbusOperationResult result);

#endif
