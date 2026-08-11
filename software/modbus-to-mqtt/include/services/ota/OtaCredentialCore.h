#ifndef MODBUS_TO_MQTT_OTACREDENTIALCORE_H
#define MODBUS_TO_MQTT_OTACREDENTIALCORE_H

#include <cstddef>
#include <cstdint>

/**
 * @file OtaCredentialCore.h
 * @brief Platform-independent primitives for creating and verifying OTA credentials.
 */

/**
 * @brief Types and functions for managing password-verifier records.
 *
 * The core delegates random-number generation and key derivation to callers so
 * that the credential logic can be used independently of the ESP32 platform.
 */
namespace OtaCredentialCore {

/** Size of a credential salt, in bytes. */
constexpr size_t SALT_SIZE = 16;

/** Size of a derived password verifier, in bytes. */
constexpr size_t VERIFIER_SIZE = 32;

/** Minimum accepted password length, in bytes. */
constexpr size_t MIN_PASSWORD_SIZE = 8;

/** Maximum accepted password length, in bytes. */
constexpr size_t MAX_PASSWORD_SIZE = 128;

// Unsupported costs are rejected before deriving. Do not raise this range
// without first measuring the worst case against the target's request budget.
/** Minimum PBKDF2 iteration count accepted in a stored record. */
constexpr uint32_t MIN_SUPPORTED_PBKDF2_ITERATIONS = 4000;

/** Maximum PBKDF2 iteration count accepted in a stored record. */
constexpr uint32_t MAX_SUPPORTED_PBKDF2_ITERATIONS = 6000;

/** PBKDF2 iteration count used when creating a record. */
constexpr uint32_t PBKDF2_ITERATIONS = 5000;

/** Current credential-record format version. */
constexpr uint8_t RECORD_VERSION = 1;

static_assert(PBKDF2_ITERATIONS >= MIN_SUPPORTED_PBKDF2_ITERATIONS
              && PBKDF2_ITERATIONS <= MAX_SUPPORTED_PBKDF2_ITERATIONS,
              "The generated OTA credential cost must be supported");

/**
 * @brief Persistent representation of an OTA password verifier.
 *
 * A record contains a salt and derived verifier, never the plaintext password.
 * The fixed-size arrays make the serialized field sizes part of the record
 * format.
 */
struct Record {
    /** Whether password protection has been configured. */
    bool configured = false;

    /** Record format version; valid records use @ref RECORD_VERSION. */
    uint8_t version = 0;

    /** Iteration count used to derive @ref verifier. */
    uint32_t iterations = 0;

    /** Per-record random salt. */
    uint8_t salt[SALT_SIZE]{};

    /** Password-derived verifier. */
    uint8_t verifier[VERIFIER_SIZE]{};
};

/**
 * @brief Callback used to derive a verifier from a password and salt.
 *
 * @param[in] password Password bytes.
 * @param[in] passwordLength Number of bytes in @p password.
 * @param[in] salt Salt bytes.
 * @param[in] saltLength Number of bytes in @p salt.
 * @param[in] iterations Derivation iteration count.
 * @param[out] output Buffer that receives the derived verifier.
 * @param[in] outputLength Required number of bytes to write to @p output.
 * @return `true` if the verifier was derived successfully; otherwise `false`.
 */
using DeriveFunction = bool (*)(const uint8_t *password, size_t passwordLength,
                                const uint8_t *salt, size_t saltLength,
                                uint32_t iterations, uint8_t *output, size_t outputLength);

/**
 * @brief Callback used to fill a buffer with cryptographically secure random bytes.
 *
 * @param[out] output Buffer to fill.
 * @param[in] outputLength Number of random bytes to write.
 * @return `true` if the complete buffer was filled; otherwise `false`.
 */
using RandomFunction = bool (*)(uint8_t *output, size_t outputLength);

/**
 * @brief Tests a password length against the credential policy.
 *
 * @param[in] passwordLength Password length in bytes.
 * @return `true` when the length is within the inclusive
 *         [@ref MIN_PASSWORD_SIZE, @ref MAX_PASSWORD_SIZE] range.
 */
bool isPasswordLengthValid(size_t passwordLength);

/**
 * @brief Validates the metadata of a credential record.
 *
 * A record is valid when it is configured, uses the current format version,
 * and specifies a supported derivation cost.
 *
 * @param[in] record Record to validate.
 * @return `true` if the record metadata is supported; otherwise `false`.
 */
bool isRecordValid(const Record &record);

/**
 * @brief Compares two byte sequences without data-dependent early termination.
 *
 * @param[in] left First sequence of at least @p length bytes.
 * @param[in] right Second sequence of at least @p length bytes.
 * @param[in] length Number of bytes to compare.
 * @return `true` if all compared bytes are equal; otherwise `false`.
 * @note Both pointers must be valid for @p length bytes.
 */
bool constantTimeEqual(const uint8_t *left, const uint8_t *right, size_t length);

/**
 * @brief Verifies a password against a credential record.
 *
 * The supplied derivation callback receives the cost and salt stored in the
 * record. The derived candidate is cleared before this function returns.
 *
 * @param[in] record Credential record to verify against.
 * @param[in] password Password bytes to verify.
 * @param[in] passwordLength Number of bytes in @p password.
 * @param[in] derive Derivation callback compatible with the stored verifier.
 * @return `true` only when the record is valid, derivation succeeds, and the
 *         derived verifier matches the stored verifier.
 */
bool verify(const Record &record, const uint8_t *password, size_t passwordLength, DeriveFunction derive);

/**
 * @brief Creates a new credential record for a password.
 *
 * The function generates a fresh salt, applies @ref PBKDF2_ITERATIONS through
 * @p derive, and replaces @p record only after all operations succeed.
 *
 * @param[out] record Record to replace on success. It remains unchanged on failure.
 * @param[in] password Password bytes from which to create the verifier.
 * @param[in] passwordLength Number of bytes in @p password.
 * @param[in] random Cryptographically secure random-byte callback.
 * @param[in] derive Password-derivation callback.
 * @return `true` if a complete record was created; `false` for invalid input,
 *         a password outside the permitted length range, or a callback failure.
 */
bool createRecord(Record &record, const uint8_t *password, size_t passwordLength,
                  RandomFunction random, DeriveFunction derive);

/**
 * @brief Overwrites all fields of a credential record with zero.
 *
 * @param[in,out] record Record to clear.
 */
void clearRecord(Record &record);

}  // namespace OtaCredentialCore

#endif
