#ifndef MODBUS_TO_MQTT_MODBUS_MQTT_WRITE_CORE_H
#define MODBUS_TO_MQTT_MODBUS_MQTT_WRITE_CORE_H

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "modbus/ModbusValueCodec.h"
#include "modbus/config_structs/ModbusDataType.h"
#include "modbus/config_structs/ModbusFunctionType.h"
#include "modbus/config_structs/ModbusWordOrder.h"

namespace ModbusMqttWriteCore {

enum class RejectionReason : uint8_t {
    None = 0,
    Empty,
    InvalidSyntax,
    NonFinite,
    InvalidScale,
    OutOfRange,
    // A TEXT register write, or FC6 with a 32-bit or 64-bit data type.
    UnsupportedDataType,
};

struct ConversionResult {
    // FC5: coil state 0/1. FC6/FC16: the first register word (words[0]).
    uint16_t rawValue{0U};
    // FC6/FC16: register words in the datapoint's word order (1, 2 or 4).
    uint8_t wordCount{0U};
    std::array<uint16_t, ModbusValueCodec::kMaxWords> words{};
    RejectionReason rejection{RejectionReason::None};

    bool accepted() const {
        return rejection == RejectionReason::None;
    }
};

struct NumberDiscoveryBounds {
    double minimum{0.0};
    double maximum{0.0};
    double step{0.0};
    RejectionReason rejection{RejectionReason::None};

    bool valid() const {
        return rejection == RejectionReason::None;
    }
};

struct CommandContext {
    uint8_t slaveId{0U};
    ModbusFunctionType function{READ_HOLDING};
    uint16_t address{0U};
    uint8_t registerCount{1U};
    double scale{1.0};
    ModbusDataType dataType{UINT16};
    ModbusWordOrder wordOrder{ModbusWordOrder::Big};
    uint32_t modbusGeneration{0U};
};

struct WriteCommand {
    std::string sourceTopic;
    uint8_t slaveId{0U};
    ModbusFunctionType function{READ_HOLDING};
    uint16_t address{0U};
    // FC5: the configured count. FC6/FC16: the number of words to write.
    uint8_t registerCount{1U};
    bool coilValue{false};
    uint8_t wordCount{0U};
    std::array<uint16_t, ModbusValueCodec::kMaxWords> words{};
    uint32_t modbusGeneration{0U};
};

using SubmitSink = std::function<void(const WriteCommand &)>;
using WarningSink = std::function<void(const std::string &)>;
using MessageHandler = std::function<void(const std::string &, const std::string &)>;

bool isWritableHoldingFunction(ModbusFunctionType function);

bool isValidWritableScale(double scale);

// Converts a command payload (engineering value text) into register words.
// Integer types: raw = value / scale must lie within the type's range
// (checked exactly in decimal), then rounds to the nearest integer with
// halves rounded upward. FLOAT32: raw must fit a float, no rounding.
ConversionResult convertPayload(ModbusFunctionType function,
                                const std::string &payload,
                                double scale,
                                ModbusDataType dataType = UINT16,
                                ModbusWordOrder wordOrder = ModbusWordOrder::Big);

// Home Assistant number bounds: the type's raw range times the scale, with
// step = |scale| (0.001 for FLOAT32), never below HA's minimum step 0.001.
NumberDiscoveryBounds numberDiscoveryBounds(double scale, ModbusDataType dataType);

const char *rejectionReasonToString(RejectionReason reason);

MessageHandler buildMessageHandler(const CommandContext &context,
                                   SubmitSink submit,
                                   WarningSink warning);

}  // namespace ModbusMqttWriteCore

#endif
