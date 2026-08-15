#ifndef MODBUS_TO_MQTT_MQTT_RUNTIME_TYPES_H
#define MODBUS_TO_MQTT_MQTT_RUNTIME_TYPES_H

#include <Arduino.h>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "mqtt/MqttConfigCore.h"

enum class MqttOperationResult : uint8_t {
    Success = 0,
    QueueFull,
    Timeout,
    Unavailable,
    Shutdown,
    Disabled,
    Disconnected,
    BrokerFailure,
    InvalidConfiguration,
    StaleGeneration,
    Superseded,
};

enum class MqttAdmissionResult : uint8_t {
    Accepted = 0,
    QueueFull,
    Unavailable,
    Shutdown,
};

struct MqttRuntimeConfiguration {
    MqttConfigCore::PreparedConnection connection;
    String rootTopic;
    String clientId;
    bool enabled{false};
};

struct MqttTopicSpec {
    MqttTopicSpec() = default;
    MqttTopicSpec(String topicValue, const bool relativeValue)
        : topic(std::move(topicValue)), relativeToRoot(relativeValue) {}

    String topic;
    bool relativeToRoot{false};
};

using MqttTopicHandler = std::function<void(const String &)>;

struct MqttSubscriptionSpec {
    MqttSubscriptionSpec() = default;
    MqttSubscriptionSpec(MqttTopicSpec topicValue, MqttTopicHandler handlerValue)
        : topic(std::move(topicValue)), handler(std::move(handlerValue)) {}

    MqttTopicSpec topic;
    MqttTopicHandler handler;
};

struct MqttWillSpec {
    bool configured{false};
    MqttTopicSpec topic;
    String payload;
    uint8_t qos{0};
    bool retain{false};
};

struct MqttBridgePlan {
    uint32_t modbusGeneration{0U};
    MqttWillSpec will;
    std::vector<MqttSubscriptionSpec> subscriptions;
};

struct MqttStatusSnapshot {
    bool available{false};
    bool enabled{false};
    bool connected{false};
    int clientState{-1};
    uint32_t generation{0U};
    uint32_t connectionEpoch{0U};
    uint32_t modbusGeneration{0U};
    uint32_t ownerViolationCount{0U};
    uint32_t commandFailureCount{0U};
    String broker;
    String user;
    String rootTopic;
    String clientId;
};

struct MqttPublishRequest {
    MqttPublishRequest() = default;
    MqttPublishRequest(String topicValue, String payloadValue, const bool retainValue,
                       const uint32_t mqttGenerationValue, const uint32_t modbusGenerationValue,
                       const uint32_t connectionEpochValue = 0U)
        : topic(std::move(topicValue)),
          payload(std::move(payloadValue)),
          retain(retainValue),
          expectedMqttGeneration(mqttGenerationValue),
          expectedModbusGeneration(modbusGenerationValue),
          expectedConnectionEpoch(connectionEpochValue) {}

    String topic;
    String payload;
    bool retain{false};
    uint32_t expectedMqttGeneration{0U};
    uint32_t expectedModbusGeneration{0U};
    uint32_t expectedConnectionEpoch{0U};
};

using MqttPublishCallback = std::function<void(MqttOperationResult)>;

struct MqttTestResult {
    MqttOperationResult operation{MqttOperationResult::Unavailable};
    bool connected{false};
    int clientState{-1};
    String broker;
    String user;
};

const char *mqttOperationResultToString(MqttOperationResult result);

const char *mqttAdmissionResultToString(MqttAdmissionResult result);

#endif
