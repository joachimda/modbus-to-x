#ifndef MODBUS_TO_MQTT_MQTTCONFIGMUTATIONCORE_H
#define MODBUS_TO_MQTT_MQTTCONFIGMUTATIONCORE_H

#include <cstddef>

#include "mqtt/MqttConfigCore.h"

namespace MqttConfigMutationCore {

enum class Status {
    Ok,
    InvalidJson,
    InvalidField,
    StorageFailed,
    ReloadFailed,
};

struct Result {
    Result(const Status statusValue = Status::Ok,
           const MqttConfigCore::ValidationError &validationValue = MqttConfigCore::ValidationError())
        : status(statusValue), validation(validationValue) {}

    Status status;
    MqttConfigCore::ValidationError validation;
};

using WriteFunction = bool (*)(const char *value, size_t length, void *context);
using ReloadFunction = bool (*)(void *context);

struct ConfigOperations {
    ConfigOperations(const WriteFunction writeValue = nullptr,
                     const ReloadFunction reloadValue = nullptr)
        : write(writeValue), reload(reloadValue) {}

    WriteFunction write;
    ReloadFunction reload;
};

struct SecretOperations {
    explicit SecretOperations(const WriteFunction writeValue = nullptr)
        : write(writeValue) {}

    WriteFunction write;
};

Result applyConfig(const char *json, size_t length, const ConfigOperations &operations, void *context);

Result applySecret(const char *json, size_t length, const SecretOperations &operations, void *context);

}  // namespace MqttConfigMutationCore

#endif
