#ifndef MODBUS_TO_MQTT_MQTTCONFIGDOCUMENT_H
#define MODBUS_TO_MQTT_MQTTCONFIGDOCUMENT_H

#include <cstddef>
#include <string>

#include "mqtt/MqttConfigCore.h"

namespace MqttConfigDocument {

struct StoredConfig {
    bool enabled = false;
    MqttConfigCore::ConnectionInput connection;
    std::string rootTopic = MqttConfigCore::DEFAULT_ROOT_TOPIC;
};

bool parseConfig(const char *json, size_t length, StoredConfig &output);

bool parsePassword(const char *json, size_t length, std::string &password);

}  // namespace MqttConfigDocument

#endif
