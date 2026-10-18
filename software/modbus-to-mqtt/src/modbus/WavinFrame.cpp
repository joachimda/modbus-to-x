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
