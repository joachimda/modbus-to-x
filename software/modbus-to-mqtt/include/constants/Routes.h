#ifndef MODBUS_TO_MQTT_ROUTES_H
#define MODBUS_TO_MQTT_ROUTES_H
class Routes {
public:
    constexpr static auto ROOT = "/";
    constexpr static auto DOWNLOAD_CFG = "/conf/config.json";
    constexpr static auto DOWNLOAD_CFG_EX = "/conf/example.json";
    constexpr static auto CONFIGURE = "/configure";
    constexpr static auto RESET_NETWORK = "/reset";
    constexpr static auto UPLOAD = "/upload";

    /*
     * API Routes
     * */
    constexpr static auto GET_SYSTEM_INFO = "/api/info";
    constexpr static auto PUT_GET_CONFIG = "/api/upload";

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
    constexpr static auto NETWORK_STATS = "/api/stats/network";
    constexpr static auto SYSTEM_STATS = "/api/stats/system";
    constexpr static auto STORAGE_STATS = "/api/stats/storage";
    constexpr static auto MODBUS_STATS = "/api/stats/modbus";
    constexpr static auto MQTT_STATS = "/api/stats/mqtt";


};
#endif