#include "services/ota/OtaCredentialCore.h"

#include <cstring>

namespace OtaCredentialCore {

bool isPasswordLengthValid(const size_t passwordLength) {
    return passwordLength >= MIN_PASSWORD_SIZE && passwordLength <= MAX_PASSWORD_SIZE;
}

bool isRecordValid(const Record &record) {
    return record.configured
           && record.version == RECORD_VERSION
           && record.iterations >= MIN_SUPPORTED_PBKDF2_ITERATIONS
           && record.iterations <= MAX_SUPPORTED_PBKDF2_ITERATIONS;
}

bool constantTimeEqual(const uint8_t *left, const uint8_t *right, const size_t length) {
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

bool verify(const Record &record, const uint8_t *password, const size_t passwordLength,
            const DeriveFunction derive) {
    if (!isRecordValid(record) || password == nullptr || derive == nullptr) return false;

    uint8_t candidate[VERIFIER_SIZE]{};
    const bool derived = derive(password, passwordLength, record.salt, sizeof(record.salt),
                                record.iterations, candidate, sizeof(candidate));
    const bool matches = derived && constantTimeEqual(candidate, record.verifier, sizeof(candidate));
    std::memset(candidate, 0, sizeof(candidate));
    return matches;
}

bool createRecord(Record &record, const uint8_t *password, const size_t passwordLength,
                  const RandomFunction random, const DeriveFunction derive) {
    if (password == nullptr || random == nullptr || derive == nullptr || !isPasswordLengthValid(passwordLength)) {
        return false;
    }

    Record candidate;
    candidate.configured = true;
    candidate.version = RECORD_VERSION;
    candidate.iterations = PBKDF2_ITERATIONS;
    if (!random(candidate.salt, sizeof(candidate.salt))
        || !derive(password, passwordLength, candidate.salt, sizeof(candidate.salt),
                   candidate.iterations, candidate.verifier, sizeof(candidate.verifier))) {
        clearRecord(candidate);
        return false;
    }
    record = candidate;
    clearRecord(candidate);
    return true;
}

void clearRecord(Record &record) {
    std::memset(&record, 0, sizeof(record));
}

}  // namespace OtaCredentialCore
