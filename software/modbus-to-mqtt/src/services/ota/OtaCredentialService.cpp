#include "services/ota/OtaCredentialService.h"

#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include "services/ota/OtaCredentialCore.h"

namespace {

constexpr const char *PREFS_NAMESPACE = "ota_auth";
constexpr const char *KEY_CONFIGURED = "set";
constexpr const char *KEY_VERSION = "version";
constexpr const char *KEY_ITERATIONS = "iterations";
constexpr const char *KEY_SALT = "salt";
constexpr const char *KEY_VERIFIER = "verifier";

bool derivePassword(const uint8_t *password, const size_t passwordLength,
                    const uint8_t *salt, const size_t saltLength,
                    const uint32_t iterations, uint8_t *output, const size_t outputLength) {
#ifdef log_d
    const uint32_t startedAt = millis();
#endif
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok = info != nullptr && mbedtls_md_setup(&context, info, 1) == 0;
    if (ok) {
        ok = mbedtls_pkcs5_pbkdf2_hmac(&context, password, passwordLength,
                                       salt, saltLength, iterations, outputLength, output) == 0;
    }
    mbedtls_md_free(&context);
#ifdef log_d
    log_d("OTA PBKDF2 (%lu iterations) completed in %lu ms",
          static_cast<unsigned long>(iterations), static_cast<unsigned long>(millis() - startedAt));
#endif
    return ok;
}

bool fillRandom(uint8_t *output, const size_t outputLength) {
    esp_fill_random(output, outputLength);
    return true;
}

bool loadRecord(OtaCredentialCore::Record &record) {
    OtaCredentialCore::clearRecord(record);
    Preferences preferences;
    if (!preferences.begin(PREFS_NAMESPACE, true)) return false;

    record.configured = preferences.getBool(KEY_CONFIGURED, false);
    if (!record.configured) {
        preferences.end();
        return true;
    }

    record.version = preferences.getUChar(KEY_VERSION, 0);
    record.iterations = preferences.getUInt(KEY_ITERATIONS, 0);
    const size_t saltLength = preferences.getBytesLength(KEY_SALT);
    const size_t verifierLength = preferences.getBytesLength(KEY_VERIFIER);
    const bool lengthsValid = saltLength == sizeof(record.salt) && verifierLength == sizeof(record.verifier);
    if (lengthsValid) {
        preferences.getBytes(KEY_SALT, record.salt, sizeof(record.salt));
        preferences.getBytes(KEY_VERIFIER, record.verifier, sizeof(record.verifier));
    }
    preferences.end();
    return lengthsValid && OtaCredentialCore::isRecordValid(record);
}

bool storeRecord(const OtaCredentialCore::Record &record) {
    Preferences preferences;
    if (!preferences.begin(PREFS_NAMESPACE, false)) return false;

    // Mark the namespace protected first. A torn write then fails closed rather than
    // silently reverting to the unprotected state.
    bool ok = preferences.putBool(KEY_CONFIGURED, true) == sizeof(bool);
    ok = preferences.putBytes(KEY_SALT, record.salt, sizeof(record.salt)) == sizeof(record.salt) && ok;
    ok = preferences.putBytes(KEY_VERIFIER, record.verifier, sizeof(record.verifier)) == sizeof(record.verifier) && ok;
    ok = preferences.putUInt(KEY_ITERATIONS, record.iterations) == sizeof(record.iterations) && ok;
    ok = preferences.putUChar(KEY_VERSION, record.version) == sizeof(record.version) && ok;
    preferences.end();
    return ok;
}

}  // namespace

bool OtaCredentialService::isProtected() {
    Preferences preferences;
    // Open read/write so a fresh device can report the unprotected state without
    // Preferences logging a missing-namespace error on every settings request.
    // A storage failure must not make protected routes appear unprotected. The
    // verifier also fails when the record cannot be loaded, so this conservative
    // result rejects requests until credential storage is available again.
    if (!preferences.begin(PREFS_NAMESPACE, false)) return true;
    const bool configured = preferences.getBool(KEY_CONFIGURED, false);
    preferences.end();
    return configured;
}

bool OtaCredentialService::verify(const String &password) {
    OtaCredentialCore::Record record;
    if (!loadRecord(record)) return false;

    const bool matches = OtaCredentialCore::verify(
        record, reinterpret_cast<const uint8_t *>(password.c_str()), password.length(), derivePassword);
    OtaCredentialCore::clearRecord(record);
    return matches;
}

OtaCredentialService::SaveResult OtaCredentialService::setPassword(const String &password) {
    OtaCredentialCore::Record record;
    if (!OtaCredentialCore::createRecord(record,
                                         reinterpret_cast<const uint8_t *>(password.c_str()), password.length(),
                                         fillRandom, derivePassword)) {
        return SaveResult::InvalidPassword;
    }
    const bool stored = storeRecord(record);
    OtaCredentialCore::clearRecord(record);
    return stored ? SaveResult::Ok : SaveResult::StorageError;
}

bool OtaCredentialService::clearPassword() {
    Preferences preferences;
    if (!preferences.begin(PREFS_NAMESPACE, false)) return false;
    const bool cleared = preferences.clear();
    preferences.end();
    return cleared;
}
