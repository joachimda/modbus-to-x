#include "mqtt/MqttConfigMutationCore.h"

#include "mqtt/MqttConfigDocument.h"

namespace MqttConfigMutationCore {

Result applyConfig(const char *json, const size_t length,
                   const ConfigOperations &operations, void *context) {
    MqttConfigDocument::StoredConfig config;
    if (json == nullptr || !MqttConfigDocument::parseConfig(json, length, config)) {
        return Result(Status::InvalidJson);
    }

    MqttConfigCore::ValidationError validation;
    if (!MqttConfigCore::validateNonSecret(config.connection, validation)) {
        return Result(Status::InvalidField, validation);
    }
    if (operations.write == nullptr || !operations.write(json, length, context)) {
        return Result(Status::StorageFailed);
    }
    if (operations.reload != nullptr && !operations.reload(context)) {
        return Result(Status::ReloadFailed);
    }
    return Result();
}

Result applySecret(const char *json, const size_t length,
                   const SecretOperations &operations, void *context) {
    std::string password;
    if (json == nullptr || !MqttConfigDocument::parsePassword(json, length, password)) {
        return Result(Status::InvalidJson);
    }

    MqttConfigCore::ValidationError validation;
    if (!MqttConfigCore::validatePassword(password, validation)) {
        return Result(Status::InvalidField, validation);
    }
    if (operations.write == nullptr || !operations.write(password.c_str(), password.size(), context)) {
        return Result(Status::StorageFailed);
    }
    return Result();
}

}  // namespace MqttConfigMutationCore
