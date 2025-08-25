#ifndef MODBUS_TO_MQTT_ROUTES_H
#define MODBUS_TO_MQTT_ROUTES_H
class Routes {
public:
    constexpr static const auto ROOT = "/";
    constexpr static const auto DOWNLOAD_CFG = "/download/config";
    constexpr static const auto DOWNLOAD_CFG_EX = "/download/config_example";
    constexpr static const auto CONFIGURE = "/configure";
    constexpr static const auto RESET_NETWORK = "/reset";
    constexpr static const auto UPLOAD = "/upload";

    /*
     * API Routes
     * */
    constexpr static const auto GET_SYSTEM_INFO = "/api/info";
    constexpr static const auto PUT_GET_CONFIG = "/api/upload";

};
#endif