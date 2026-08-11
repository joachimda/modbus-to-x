#include "services/ota/OtaAuthorizationCore.h"

#include <cstring>

#include "services/ota/OtaCredentialCore.h"

namespace {

int base64Value(const char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

}  // namespace

bool OtaAuthorizationCore::decodeBearerPassword(const char *encoded, const size_t encodedLength,
                                                uint8_t *password, const size_t passwordCapacity,
                                                size_t &passwordLength) {
    passwordLength = 0;
    if (encoded == nullptr || password == nullptr || encodedLength == 0U || encodedLength % 4U != 0U) return false;

    for (size_t offset = 0; offset < encodedLength; offset += 4U) {
        const bool last = offset + 4U == encodedLength;
        const int first = base64Value(encoded[offset]);
        const int second = base64Value(encoded[offset + 1U]);
        const int third = encoded[offset + 2U] == '=' ? -2 : base64Value(encoded[offset + 2U]);
        const int fourth = encoded[offset + 3U] == '=' ? -2 : base64Value(encoded[offset + 3U]);
        if (first < 0 || second < 0 || third == -1 || fourth == -1) return false;
        if ((third == -2 && fourth != -2) || ((third == -2 || fourth == -2) && !last)) return false;

        const size_t bytes = third == -2 ? 1U : (fourth == -2 ? 2U : 3U);
        if (passwordLength + bytes > passwordCapacity) return false;
        password[passwordLength++] = static_cast<uint8_t>((first << 2) | (second >> 4));
        if (bytes > 1U) password[passwordLength++] = static_cast<uint8_t>((second << 4) | (third >> 2));
        if (bytes > 2U) password[passwordLength++] = static_cast<uint8_t>((third << 6) | fourth);
    }
    return true;
}

bool OtaAuthorizationCore::authorize(const bool protectedMode, const char *encoded, const size_t encodedLength,
                                     const VerifyFunction verify, void *context) {
    if (!protectedMode) return true;
    if (encodedLength > 172U || verify == nullptr) return false;

    uint8_t password[OtaCredentialCore::MAX_PASSWORD_SIZE]{};
    size_t passwordLength = 0;
    const bool decoded = decodeBearerPassword(
        encoded, encodedLength, password, sizeof(password), passwordLength);
    const bool authorized = decoded && OtaCredentialCore::isPasswordLengthValid(passwordLength)
                            && verify(password, passwordLength, context);
    std::memset(password, 0, sizeof(password));
    return authorized;
}
