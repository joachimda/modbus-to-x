#ifndef MODBUS_TO_MQTT_OTA_SESSION_CORE_H
#define MODBUS_TO_MQTT_OTA_SESSION_CORE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace OtaSessionCore {

constexpr size_t TOKEN_BYTES = 32U;

inline bool elapsedAtLeast(const uint32_t now, const uint32_t then, const uint32_t duration) {
    return static_cast<uint32_t>(now - then) >= duration;
}

template<size_t Capacity>
class Store {
public:
    Store(const uint32_t absoluteLifetimeMs, const uint32_t idleLifetimeMs)
        : _absoluteLifetimeMs(absoluteLifetimeMs), _idleLifetimeMs(idleLifetimeMs) {}

    bool create(const uint8_t token[TOKEN_BYTES], const uint32_t now) {
        expire(now);
        for (auto &slot : _slots) {
            if (slot.active) continue;
            std::memcpy(slot.token.data(), token, TOKEN_BYTES);
            slot.createdAt = now;
            slot.lastUsedAt = now;
            slot.active = true;
            return true;
        }
        return false;
    }

    bool authorize(const uint8_t token[TOKEN_BYTES], const uint32_t now) {
        expire(now);
        size_t matched = Capacity;
        for (size_t i = 0U; i < Capacity; ++i) {
            uint8_t difference = 0U;
            for (size_t j = 0U; j < TOKEN_BYTES; ++j) {
                difference |= static_cast<uint8_t>(_slots[i].token[j] ^ token[j]);
            }
            if (_slots[i].active && difference == 0U) matched = i;
        }
        if (matched == Capacity) return false;
        _slots[matched].lastUsedAt = now;
        return true;
    }

    void revokeAll() {
        for (auto &slot : _slots) clear(slot);
    }

    void expire(const uint32_t now) {
        for (auto &slot : _slots) {
            if (!slot.active) continue;
            if (elapsedAtLeast(now, slot.createdAt, _absoluteLifetimeMs)
                || elapsedAtLeast(now, slot.lastUsedAt, _idleLifetimeMs)) clear(slot);
        }
    }

    size_t activeCount() const {
        size_t count = 0U;
        for (const auto &slot : _slots) if (slot.active) ++count;
        return count;
    }

private:
    struct Slot {
        std::array<uint8_t, TOKEN_BYTES> token{};
        uint32_t createdAt{0U};
        uint32_t lastUsedAt{0U};
        bool active{false};
    };

    static void clear(Slot &slot) {
        volatile uint8_t *bytes = slot.token.data();
        for (size_t i = 0U; i < TOKEN_BYTES; ++i) bytes[i] = 0U;
        slot.createdAt = 0U;
        slot.lastUsedAt = 0U;
        slot.active = false;
    }

    std::array<Slot, Capacity> _slots{};
    uint32_t _absoluteLifetimeMs;
    uint32_t _idleLifetimeMs;
};

inline bool decodeHexToken(const char *text, const size_t length, uint8_t output[TOKEN_BYTES]) {
    if (text == nullptr || output == nullptr || length != TOKEN_BYTES * 2U) return false;
    auto nibble = [](const char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (size_t i = 0U; i < TOKEN_BYTES; ++i) {
        const int high = nibble(text[i * 2U]);
        const int low = nibble(text[i * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        output[i] = static_cast<uint8_t>((high << 4U) | low);
    }
    return true;
}

inline void encodeHexToken(const uint8_t token[TOKEN_BYTES], char output[TOKEN_BYTES * 2U + 1U]) {
    constexpr char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t i = 0U; i < TOKEN_BYTES; ++i) {
        output[i * 2U] = HEX_DIGITS[token[i] >> 4U];
        output[i * 2U + 1U] = HEX_DIGITS[token[i] & 0x0FU];
    }
    output[TOKEN_BYTES * 2U] = '\0';
}

}  // namespace OtaSessionCore

#endif
