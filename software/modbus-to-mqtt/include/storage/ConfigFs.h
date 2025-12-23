#ifndef CONFIG_FS_H
#define CONFIG_FS_H

#include <SPIFFS.h>

extern fs::SPIFFSFS ConfigFS;

namespace ConfigFs {
constexpr const char *kBasePath = "/conf";
constexpr const char *kPartitionLabel = "cfg";
constexpr const char *kModbusConfigFile = "/config.json";
constexpr const char *kMqttConfigFile = "/mqtt.json";
}

#endif
