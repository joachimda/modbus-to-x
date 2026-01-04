#ifndef MODBUS_TO_MQTT_OTA_PUBLIC_KEY_H
#define MODBUS_TO_MQTT_OTA_PUBLIC_KEY_H
#pragma once

struct OtaPubKey {
    const char *kid;
    const char *pem;
};

static constexpr OtaPubKey OTA_PUBKEYS[] = {
    {
        "bd774d27-badf-48a3-b927-a51fb08629f7",
        R"PEM(
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEjyyJNAC1x2OMdCdjC5cIyJPHXIfa
G5VTvecJgxbR3NR5ZtcW1ruCNY+MWIsOG74I8n61GrB0ZGdD2J6gGnIS7Q==
-----END PUBLIC KEY-----
)PEM"
    },
};

static constexpr auto OTA_PUBKEYS_COUNT = sizeof(OTA_PUBKEYS) / sizeof(OTA_PUBKEYS[0]);

inline const char *ota_find_pubkey_pem_by_kid(const char *kid) {
    if (!kid) return nullptr;
    for (const auto i: OTA_PUBKEYS) {
        const char *a = i.kid;
        const char *b = kid;
        while (*a && *b && (*a == *b)) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') return i.pem;
    }
    return nullptr;
}
#endif //MODBUS_TO_MQTT_OTA_PUBLIC_KEY_H
