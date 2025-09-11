#ifndef MODBUS_TO_MQTT_STATSERVICE_H
#define MODBUS_TO_MQTT_STATSERVICE_H
#include <ArduinoJson.h>

#include "Logger.h"

class StatService {
  public:
  static JsonDocument appendSystemStats(JsonDocument & document, const Logger *logger);

  static JsonDocument appendNetworkStats(JsonDocument &document);

  static JsonDocument appendMQTTStats(JsonDocument &document);
  static JsonDocument appendModbusStats(JsonDocument &document);
  static JsonDocument appendStorageStats(JsonDocument &document);
  static JsonDocument appendHealthStats(JsonDocument &document);
};
#endif //MODBUS_TO_MQTT_STATSERVICE_H
