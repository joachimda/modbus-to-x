#ifndef MODBUS_TO_MQTT_TEST_MBEDTLS_PKCS5_H
#define MODBUS_TO_MQTT_TEST_MBEDTLS_PKCS5_H

#include <cstddef>
#include <cstdint>

#include "md.h"

inline int mbedtls_pkcs5_pbkdf2_hmac(mbedtls_md_context_t *,
                                    const unsigned char *password, const size_t passwordLength,
                                    const unsigned char *salt, const size_t saltLength,
                                    const unsigned int iterations, const uint32_t outputLength,
                                    unsigned char *output) {
    if (password == nullptr || passwordLength == 0U || salt == nullptr || saltLength == 0U || output == nullptr) {
        return -1;
    }
    for (size_t index = 0; index < outputLength; ++index) {
        output[index] = password[index % passwordLength] ^ salt[index % saltLength]
                        ^ static_cast<uint8_t>(iterations >> ((index % 4U) * 8U));
    }
    return 0;
}

#endif
