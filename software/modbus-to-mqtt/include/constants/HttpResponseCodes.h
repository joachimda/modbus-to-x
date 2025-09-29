#ifndef MODBUS_TO_MQTT_HTTPRESPONSECODES_H
#define MODBUS_TO_MQTT_HTTPRESPONSECODES_H

class HttpResponseCodes {
public:
    static constexpr int OK = 200;
    static constexpr int ACCEPTED = 202;
    static constexpr int NO_CONTENT = 204;
    static constexpr int REDIRECT = 302;
    static constexpr int BAD_REQUEST = 403;
    static constexpr int NOT_FOUND = 404;
    static constexpr int CONFLICT = 409;
    static constexpr int INTERNAL_SERVER_ERROR = 500;
    static constexpr int SERVICE_UNAVAILABLE = 503;
};

#endif
