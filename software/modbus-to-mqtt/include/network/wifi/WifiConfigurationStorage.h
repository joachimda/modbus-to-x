#ifndef MODBUS_TO_MQTT_WIFICONFIGURATIONSTORAGE_H
#define MODBUS_TO_MQTT_WIFICONFIGURATIONSTORAGE_H

#include "network/wifi/WifiConfiguration.h"

class WifiConfigurationStorage {
public:
    enum class LoadResult : uint8_t {
        NotFound,
        Dhcp,
        Static,
        Invalid,
        Unavailable,
    };

    static bool save(const WifiStaticConfig &configuration);
    static LoadResult load(WifiStaticConfig &configuration);
    static bool clear();
};

#endif
