#ifndef MODBUS_TO_MQTT_ROUTES_H
#define MODBUS_TO_MQTT_ROUTES_H
class Routes {
public:
    constexpr static auto ROOT = "/";
    constexpr static auto CONFIGURE = "/configure";
    constexpr static auto RESET_NETWORK = "/reset";
    constexpr static auto GET_MODBUS_CONFIG = "/conf/config.json";
    constexpr static auto GET_MQTT_CONFIG = "/conf/mqtt.json";

    /* *
     * * Modbus Config API
     * * */
    constexpr static auto PUT_MODBUS_CONFIG = "/api/config/modbus";
    constexpr static auto PUT_MQTT_CONFIG = "/api/config/mqtt";
    constexpr static auto PUT_MQTT_SECRET = "/api/config/mqtt/secret";

    /* *
     * * WIFI API
     * * */
    constexpr static auto GET_SSID_LIST = "/api/ssids";
    constexpr static auto GET_WIFI_STATUS = "/api/wifi/status";
    constexpr static auto POST_WIFI_AP_OFF = "/api/wifi/ap_off";
    constexpr static auto POST_WIFI_CANCEL = "/api/wifi/cancel";
    constexpr static auto POST_WIFI_CONNECT = "/api/wifi/connect";

    /* *
     * * STATS API
     * * */
    constexpr static auto SYSTEM_STATS = "/api/stats/system";
    constexpr static auto LOGS = "/api/logs";
    constexpr static auto DEVICE_RESET = "/api/system/reboot";
    constexpr static auto MQTT_TEST_CONNECT = "/api/mqtt/test";

    /* *
     * * OTA API
     * * */
    constexpr static auto OTA_FIRMWARE = "/api/system/ota/firmware";
    constexpr static auto OTA_FILESYSTEM = "/api/system/ota/fs";
};
#endif
