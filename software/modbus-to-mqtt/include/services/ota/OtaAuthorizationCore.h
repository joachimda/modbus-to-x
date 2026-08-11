#ifndef MODBUS_TO_MQTT_OTAAUTHORIZATIONCORE_H
#define MODBUS_TO_MQTT_OTAAUTHORIZATIONCORE_H

#include <cstddef>
#include <cstdint>

/**
 * @file OtaAuthorizationCore.h
 * @brief Platform-independent decoding and authorization of OTA bearer credentials.
 */

/** @brief Helpers for authorizing OTA requests with Base64-encoded passwords. */
namespace OtaAuthorizationCore {

/**
 * @brief Callback used to verify a decoded bearer password.
 *
 * @param[in] password Decoded password bytes; the data is not null-terminated.
 * @param[in] passwordLength Number of bytes in @p password.
 * @param[in,out] context Opaque caller-provided context.
 * @return `true` if the password is accepted; otherwise `false`.
 */
using VerifyFunction = bool (*)(const uint8_t *password, size_t passwordLength, void *context);

/**
 * @brief Decodes the Base64 payload of an OTA bearer credential.
 *
 * Input must be non-empty, use the standard Base64 alphabet, consist of
 * complete four-character groups, and place any `=` padding in the final
 * group. The input is the bearer value itself and must not include a `Bearer `
 * scheme prefix. No null terminator is appended to the decoded bytes.
 *
 * @param[in] encoded Base64-encoded password bytes.
 * @param[in] encodedLength Number of characters in @p encoded.
 * @param[out] password Destination for the decoded password.
 * @param[in] passwordCapacity Capacity of @p password in bytes.
 * @param[out] passwordLength Number of decoded bytes written on success.
 * @return `true` if the entire value was decoded; `false` for invalid input or
 *         insufficient destination capacity.
 * @note On failure, @p password and @p passwordLength may contain a partial
 *       result and must be discarded by the caller.
 */
bool decodeBearerPassword(const char *encoded, size_t encodedLength,
                          uint8_t *password, size_t passwordCapacity, size_t &passwordLength);

/**
 * @brief Authorizes an OTA request using an encoded bearer password.
 *
 * When protection is enabled, this function decodes the credential, enforces
 * the OTA password-length policy, and invokes @p verify. Temporary decoded
 * password storage is cleared before returning.
 *
 * @param[in] protectedMode Whether the OTA password requirement is enabled.
 * @param[in] encoded Base64-encoded password, without the `Bearer ` prefix.
 * @param[in] encodedLength Number of characters in @p encoded.
 * @param[in] verify Password-verification callback; required in protected mode.
 * @param[in,out] context Opaque value passed unchanged to @p verify.
 * @return `true` immediately when @p protectedMode is `false`; in protected
 *         mode, `true` only when decoding and password verification succeed.
 */
bool authorize(bool protectedMode, const char *encoded, size_t encodedLength,
               VerifyFunction verify, void *context);

}  // namespace OtaAuthorizationCore

#endif
