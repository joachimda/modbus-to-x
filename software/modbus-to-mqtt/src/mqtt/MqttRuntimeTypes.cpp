#include "mqtt/MqttRuntimeTypes.h"

const char *mqttOperationResultToString(const MqttOperationResult result) {
    switch (result) {
        case MqttOperationResult::Success: return "success";
        case MqttOperationResult::QueueFull: return "queue_full";
        case MqttOperationResult::Timeout: return "timeout";
        case MqttOperationResult::Unavailable: return "unavailable";
        case MqttOperationResult::Shutdown: return "shutdown";
        case MqttOperationResult::Disabled: return "disabled";
        case MqttOperationResult::Disconnected: return "disconnected";
        case MqttOperationResult::BrokerFailure: return "broker_failure";
        case MqttOperationResult::InvalidConfiguration: return "invalid_configuration";
        case MqttOperationResult::StaleGeneration: return "stale_generation";
        case MqttOperationResult::Superseded: return "superseded";
    }
    return "unknown";
}

const char *mqttAdmissionResultToString(const MqttAdmissionResult result) {
    switch (result) {
        case MqttAdmissionResult::Accepted: return "accepted";
        case MqttAdmissionResult::QueueFull: return "queue_full";
        case MqttAdmissionResult::Unavailable: return "unavailable";
        case MqttAdmissionResult::Shutdown: return "shutdown";
    }
    return "unknown";
}
