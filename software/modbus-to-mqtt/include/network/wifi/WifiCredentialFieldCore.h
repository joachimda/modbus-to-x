#ifndef MODBUS_TO_MQTT_WIFICREDENTIALFIELDCORE_H
#define MODBUS_TO_MQTT_WIFICREDENTIALFIELDCORE_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WifiCredentialFieldCore {

inline bool copy(std::uint8_t *destination, const std::size_t capacity,
                 const char *source, const std::size_t length) {
    if (destination == nullptr || source == nullptr || length > capacity) return false;
    std::memset(destination, 0, capacity);
    if (length != 0U) std::memcpy(destination, source, length);
    return true;
}

inline bool equals(const std::uint8_t *field, const std::size_t capacity,
                   const char *expected, const std::size_t length) {
    if (field == nullptr || expected == nullptr || length > capacity) return false;
    if (length != 0U && std::memcmp(field, expected, length) != 0) return false;
    return length == capacity || field[length] == 0U;
}

}  // namespace WifiCredentialFieldCore

#endif
