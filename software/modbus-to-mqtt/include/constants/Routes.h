#ifndef MODBUS_TO_MQTT_ROUTES_H
#define MODBUS_TO_MQTT_ROUTES_H
class Routes {
public:
    constexpr static auto ROOT = "/";
    constexpr static auto DOWNLOAD_CFG = "/download/config";
    constexpr static auto DOWNLOAD_CFG_EX = "/download/config_example";
    constexpr static auto CONFIGURE = "/configure";
    constexpr static auto RESET_NETWORK = "/reset";
    constexpr static auto UPLOAD = "/upload";

    /*
     * API Routes
     * */
    constexpr static auto GET_SYSTEM_INFO = "/api/info";
    constexpr static auto GET_WIFI_STATUS = "/api/wifi/status";
    constexpr static auto PUT_GET_CONFIG = "/api/upload";
    constexpr static auto GET_SSID_LIST = "/api/ssids";
    constexpr static auto POST_WIFI_AP_OFF = "/api/wifi/ap_off";
    constexpr static auto POST_WIFI_CANCEL = "/api/wifi/cancel";
    constexpr static auto POST_WIFI_CONNECT = "/api/wifi/connect";


};
#endif