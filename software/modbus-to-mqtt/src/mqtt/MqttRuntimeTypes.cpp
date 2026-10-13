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
        case MqttOperationResult::OtaInProgress: return "ota_in_progress";
    }
    return "unknown";
}

const char *mqttAdmissionResultToString(const MqttAdmissionResult result) {
    switch (result) {
        case MqttAdmissionResult::Accepted: return "accepted";
        case MqttAdmissionResult::QueueFull: return "queue_full";
        case MqttAdmissionResult::Unavailable: return "unavailable";
        case MqttAdmissionResult::Shutdown: return "shutdown";
        case MqttAdmissionResult::OtaInProgress: return "ota_in_progress";
    }
    return "unknown";
}

const char *mqttOtaQuiesceResultToString(const MqttOtaQuiesceResult result) {
    switch (result) {
        case MqttOtaQuiesceResult::None: return "none";
        case MqttOtaQuiesceResult::Signalled: return "signalled";
        case MqttOtaQuiesceResult::AlreadyDisconnected: return "already_disconnected";
        case MqttOtaQuiesceResult::Disabled: return "disabled";
        case MqttOtaQuiesceResult::NoSignalConfigured: return "no_signal_configured";
        case MqttOtaQuiesceResult::SignalFailed: return "signal_failed";
    }
    return "none";
}
