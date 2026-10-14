#ifndef MODBUS_TO_MQTT_MODBUS_WORD_ORDER_H
#define MODBUS_TO_MQTT_MODBUS_WORD_ORDER_H

#include <cstdint>

// Register (16-bit word) order of multi-register values. Bytes inside a
// register are always big-endian on the wire, as Modbus specifies.
//   Big:    the first (lowest-address) register holds the most significant word.
//   Little: the first register holds the least significant word.
enum class ModbusWordOrder : uint8_t {
    Big = 0,
    Little = 1
};

#endif
