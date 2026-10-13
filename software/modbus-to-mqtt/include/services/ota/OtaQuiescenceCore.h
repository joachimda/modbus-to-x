#ifndef MODBUS_TO_MQTT_OTA_QUIESCENCE_CORE_H
#define MODBUS_TO_MQTT_OTA_QUIESCENCE_CORE_H

#include <cstdint>

namespace OtaQuiescenceCore {

enum class Result : uint8_t {
    Pending = 0,
    Ready,
    MqttTimeout,
    ModbusTimeout,
    HttpTimeout,
};

inline Result evaluate(const uint32_t startedAt, const uint32_t now,
                       const bool mqttReady, const bool modbusReady, const bool httpReady,
                       const uint32_t mqttTimeout, const uint32_t modbusTimeout,
                       const uint32_t httpTimeout) {
    if (mqttReady && modbusReady && httpReady) return Result::Ready;
    const uint32_t elapsed = now - startedAt;
    if (!mqttReady && elapsed >= mqttTimeout) return Result::MqttTimeout;
    if (!modbusReady && elapsed >= modbusTimeout) return Result::ModbusTimeout;
    if (!httpReady && elapsed >= httpTimeout) return Result::HttpTimeout;
    return Result::Pending;
}

inline const char *errorCode(const Result result) {
    switch (result) {
        case Result::MqttTimeout: return "mqtt_quiesce_timeout";
        case Result::ModbusTimeout: return "modbus_quiesce_timeout";
        case Result::HttpTimeout: return "http_quiesce_timeout";
        case Result::Pending:
        case Result::Ready:
            return "";
    }
    return "";
}

}  // namespace OtaQuiescenceCore

#endif
