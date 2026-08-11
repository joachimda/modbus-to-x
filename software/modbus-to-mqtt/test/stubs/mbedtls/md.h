#ifndef MODBUS_TO_MQTT_TEST_MBEDTLS_MD_H
#define MODBUS_TO_MQTT_TEST_MBEDTLS_MD_H

struct mbedtls_md_context_t {};
struct mbedtls_md_info_t {};

constexpr int MBEDTLS_MD_SHA256 = 1;

inline void mbedtls_md_init(mbedtls_md_context_t *) {}
inline void mbedtls_md_free(mbedtls_md_context_t *) {}
inline const mbedtls_md_info_t *mbedtls_md_info_from_type(int) {
    static mbedtls_md_info_t info;
    return &info;
}
inline int mbedtls_md_setup(mbedtls_md_context_t *, const mbedtls_md_info_t *, int) {
    return 0;
}

#endif
