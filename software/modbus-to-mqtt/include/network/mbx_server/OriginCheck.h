#ifndef MODBUS_TO_MQTT_NETWORK_ORIGIN_CHECK_H
#define MODBUS_TO_MQTT_NETWORK_ORIGIN_CHECK_H

#include <Arduino.h>

inline String normalizeOriginHost(String s) {
    s.toLowerCase();
    const int sep = s.indexOf("://");
    if (sep >= 0) s = s.substring(sep + 3);
    if (s.endsWith(":80")) s = s.substring(0, s.length() - 3);
    return s;
}

inline bool isOriginAllowed(const String &origin, const String &host) {
    if (origin.length() == 0) return true;
    if (origin == "null") return false;
    return normalizeOriginHost(origin) == normalizeOriginHost(host);
}

#endif
