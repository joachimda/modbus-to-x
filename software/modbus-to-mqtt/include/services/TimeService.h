#ifndef MODBUS_TO_MQTT_TIMESERVICE_H
#define MODBUS_TO_MQTT_TIMESERVICE_H
#pragma once

#include <Arduino.h>

class TimeService {
public:
    static void requestSync();
    static void loop();
    static bool hasValidTime();
    static String nowIso();
    static String formatIso(time_t t);
};

#endif // MODBUS_TO_MQTT_TIMESERVICE_H
