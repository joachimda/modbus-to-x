#include "modbus/WavinFrame.h"

namespace WavinFrame {

size_t buildRead(const uint8_t slave, const uint8_t category, const uint8_t index,
                 const uint8_t page, const uint8_t count, uint8_t *out, const size_t capacity) {
    if (out == nullptr || capacity < kReadRequestSize || count == 0U || count > kMaxRegisters) {
        return 0U;
    }
    out[0] = slave;
    out[1] = kReadFunction;
    out[2] = category;
    out[3] = index;
    out[4] = page;
    out[5] = count;
    return RtuFrame::appendCrc(out, 6U);
}

size_t buildWrite(const uint8_t slave, const uint8_t category, const uint8_t index,
                  const uint8_t page, const uint16_t *words, const uint8_t count,
                  uint8_t *out, const size_t capacity) {
    const size_t size = 6U + 2U * static_cast<size_t>(count) + RtuFrame::kCrcSize;
    if (out == nullptr || words == nullptr || count == 0U || size > kMaxWriteRequestSize
        || capacity < size) {
        return 0U;
    }
    out[0] = slave;
    out[1] = kWriteFunction;
    out[2] = category;
    out[3] = index;
    out[4] = page;
    out[5] = count;
    for (uint8_t i = 0U; i < count; ++i) {
        out[6U + 2U * i] = static_cast<uint8_t>(words[i] >> 8U);
        out[7U + 2U * i] = static_cast<uint8_t>(words[i] & 0xFFU);
    }
    return RtuFrame::appendCrc(out, size - RtuFrame::kCrcSize);
}

bool parseWordList(const char *text, uint16_t *words, const size_t capacity, uint8_t &count) {
    count = 0U;
    if (text == nullptr || words == nullptr) return false;
    const size_t limit = capacity < kMaxRegisters ? capacity : kMaxRegisters;
    uint8_t parsed = 0U;
    const char *cursor = text;
    for (;;) {
        while (*cursor == ' ') ++cursor;
        unsigned base = 10U;
        if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
            base = 16U;
            cursor += 2;
        }
        uint32_t value = 0U;
        size_t digits = 0U;
        for (;; ++cursor, ++digits) {
            const char c = *cursor;
            unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (base == 16U && c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (base == 16U && c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else break;
            value = value * base + digit;
            if (value > 0xFFFFU) return false;
        }
        if (digits == 0U || parsed >= limit) return false;
        words[parsed++] = static_cast<uint16_t>(value);
        while (*cursor == ' ') ++cursor;
        if (*cursor == '\0') break;
        if (*cursor != ',') return false;
        ++cursor;
    }
    count = parsed;
    return true;
}

uint8_t decodeReply(const uint8_t *frame, const size_t length, uint16_t *words,
                    const size_t wordCapacity) {
    if (frame == nullptr || length < responseSize(0U)) return 0U;
    const size_t byteCount = frame[2];
    if (length < kReplyHeaderSize + byteCount + RtuFrame::kCrcSize) return 0U;
    const size_t wordCount = byteCount / 2U;
    for (size_t i = 0U; i < wordCount && words != nullptr && i < wordCapacity; ++i) {
        words[i] = static_cast<uint16_t>((frame[kReplyHeaderSize + 2U * i] << 8U)
                                         | frame[kReplyHeaderSize + 2U * i + 1U]);
    }
    return static_cast<uint8_t>(wordCount);
}

}  // namespace WavinFrame
