#ifndef MODBUS_TO_MQTT_OTASERVICE_H
#define MODBUS_TO_MQTT_OTASERVICE_H

#include <Arduino.h>

class Logger;

class OtaService {
public:
    static bool beginFirmware(size_t totalSize, const Logger *logger);
    static bool write(uint8_t *data, size_t len, const Logger *logger);
    static bool end(bool evenIfHasError, const Logger *logger);

    static bool beginFilesystem(size_t totalSize, const Logger *logger);
};

#endif

