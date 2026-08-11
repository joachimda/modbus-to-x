#ifndef MODBUS_TO_MQTT_OTACREDENTIALSERVICE_H
#define MODBUS_TO_MQTT_OTACREDENTIALSERVICE_H

#include <Arduino.h>

/**
 * @file OtaCredentialService.h
 * @brief Persistent OTA password management for the ESP32 platform.
 */

/**
 * @brief Stores and verifies the password used to protect OTA operations.
 *
 * The service persists a salted PBKDF2-HMAC-SHA-256 verifier in non-volatile
 * storage. Plaintext passwords are not stored.
 */
class OtaCredentialService {
public:
    /** @brief Result of attempting to create and persist a password verifier. */
    enum class SaveResult : uint8_t {
        /** The credential record was created and persisted. */
        Ok,

        /** The password was invalid or its verifier could not be generated. */
        InvalidPassword,

        /** The credential record could not be persisted. */
        StorageError,
    };

    /**
     * @brief Reports whether OTA operations must be authenticated.
     *
     * Failure to open credential storage is treated as protected mode so that
     * a storage fault cannot silently expose OTA operations.
     *
     * @return `true` when a password is configured or storage is unavailable;
     *         otherwise `false`.
     */
    static bool isProtected();

    /**
     * @brief Verifies a candidate password against the persisted credential.
     *
     * @param[in] password Candidate password.
     * @return `true` if a valid credential record exists and the password
     *         matches; otherwise `false`, including on storage or derivation errors.
     */
    static bool verify(const String &password);

    /**
     * @brief Sets or replaces the OTA password.
     *
     * A new random salt and verifier are generated for every successful call.
     * Password length is measured in bytes and must satisfy the limits declared
     * by OtaCredentialCore.
     *
     * @param[in] password New OTA password.
     * @return A status describing whether the credential was saved.
     */
    static SaveResult setPassword(const String &password);

    /**
     * @brief Removes all persisted OTA credential data.
     *
     * OTA operations are unprotected after this function succeeds.
     *
     * @return `true` if the credential namespace was cleared; otherwise `false`.
     */
    static bool clearPassword();
};

#endif
