#include "mqtt/MqttConfigDocument.h"

#include <ArduinoJson.h>
#include <utility>

namespace MqttConfigDocument {
namespace {

std::string jsonString(const JsonDocument &document, const char *key, const char *fallback = "") {
    const char *value = document[key] | fallback;
    return value == nullptr ? std::string(fallback) : std::string(value);
}

}  // namespace

bool parseConfig(const char *json, const size_t length, StoredConfig &output) {
    JsonDocument document;
    if (deserializeJson(document, json, length)) return false;

    StoredConfig candidate;
    candidate.enabled = document["enabled"] | false;
    candidate.connection.brokerIp = jsonString(document, "broker_ip");
    candidate.connection.brokerUrl = jsonString(document, "broker_url");
    candidate.connection.brokerPort = jsonString(document, "broker_port", MqttConfigCore::DEFAULT_PORT);
    candidate.connection.user = jsonString(document, "user");
    candidate.rootTopic = jsonString(document, "root_topic", MqttConfigCore::DEFAULT_ROOT_TOPIC);
    output = std::move(candidate);
    return true;
}

bool parsePassword(const char *json, const size_t length, std::string &password) {
    JsonDocument document;
    if (deserializeJson(document, json, length)) return false;
    password = jsonString(document, "password");
    return true;
}

}  // namespace MqttConfigDocument
