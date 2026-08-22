#ifndef MODBUS_TO_MQTT_MODBUS_MQTT_WRITE_CORE_H
#define MODBUS_TO_MQTT_MODBUS_MQTT_WRITE_CORE_H

#include <cstdint>
#include <functional>
#include <string>

#include "modbus/config_structs/ModbusFunctionType.h"

namespace ModbusMqttWriteCore {

enum class RejectionReason : uint8_t {
    None = 0,
    Empty,
    InvalidSyntax,
    NonFinite,
    InvalidScale,
    OutOfRange,
};

struct ConversionResult {
    uint16_t rawValue{0U};
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
    uint32_t modbusGeneration{0U};
};

struct WriteCommand {
    std::string sourceTopic;
    uint8_t slaveId{0U};
    ModbusFunctionType function{READ_HOLDING};
    uint16_t address{0U};
    uint8_t registerCount{1U};
    uint16_t rawValue{0U};
    uint32_t modbusGeneration{0U};
};

using SubmitSink = std::function<void(const WriteCommand &)>;
using WarningSink = std::function<void(const std::string &)>;
using MessageHandler = std::function<void(const std::string &, const std::string &)>;

bool isWritableHoldingFunction(ModbusFunctionType function);

bool isValidWritableScale(double scale);

ConversionResult convertPayload(ModbusFunctionType function,
                                const std::string &payload,
                                double scale);

NumberDiscoveryBounds numberDiscoveryBounds(double scale);

const char *rejectionReasonToString(RejectionReason reason);

MessageHandler buildMessageHandler(const CommandContext &context,
                                   SubmitSink submit,
                                   WarningSink warning);

}  // namespace ModbusMqttWriteCore

#endif
