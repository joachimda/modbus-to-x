#ifndef MODBUS_TO_MQTT_REGISTER_SLICE_H
#define MODBUS_TO_MQTT_REGISTER_SLICE_H

#include <cstdint>

// Which part of a single 16-bit register holds the value. Only meaningful for
// the 16-bit data types; multi-register types always use the full registers.
enum class RegisterSlice : uint8_t {
    Full = 0,
    LowByte,
    HighByte
};

#endif
