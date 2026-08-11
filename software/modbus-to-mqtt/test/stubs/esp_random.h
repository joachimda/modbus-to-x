#ifndef MODBUS_TO_MQTT_TEST_ESP_RANDOM_H
#define MODBUS_TO_MQTT_TEST_ESP_RANDOM_H

#include <cstddef>
#include <cstdint>

inline void esp_fill_random(void *output, const size_t length) {
    auto *bytes = static_cast<uint8_t *>(output);
    static uint8_t next = 1U;
    for (size_t index = 0; index < length; ++index) bytes[index] = next++;
}

#endif
