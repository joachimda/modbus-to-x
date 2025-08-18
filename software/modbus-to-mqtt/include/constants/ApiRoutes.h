#ifndef MODBUS_TO_MQTT_APIROUTES_H
#define MODBUS_TO_MQTT_APIROUTES_H
class ApiRoutes {
public:
    constexpr static const auto ROOT = "/";
    constexpr static const auto DOWNLOAD_CFG = "/download/config";
    constexpr static const auto DOWNLOAD_CFG_EX = "/download/config_example";
    constexpr static const auto CONFIGURE = "/configure";
    constexpr static const auto UPLOAD = "/upload";
};
#endif