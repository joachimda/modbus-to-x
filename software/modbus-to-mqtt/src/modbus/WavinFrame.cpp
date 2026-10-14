#include "modbus/WavinFrame.h"

namespace WavinFrame {
namespace {

constexpr size_t kHeaderSize = 3U;
constexpr size_t kCrcSize = 2U;
constexpr size_t kExceptionSize = 5U;

size_t appendCrc(uint8_t *frame, const size_t length) {
    const uint16_t crc = crc16(frame, length);
    frame[length] = static_cast<uint8_t>(crc & 0xFFU);
    frame[length + 1U] = static_cast<uint8_t>(crc >> 8U);
    return length + kCrcSize;
}

// A frame's CRC over all of its bytes, CRC included, is zero when intact.
bool crcValid(const uint8_t *frame, const size_t length) {
    return crc16(frame, length) == 0U;
}

}  // namespace

uint16_t crc16(const uint8_t *data, const size_t length) {
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x0001U) ? static_cast<uint16_t>((crc >> 1U) ^ 0xA001U)
                                  : static_cast<uint16_t>(crc >> 1U);
        }
    }
    return crc;
}

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
    return appendCrc(out, 6U);
}

size_t buildWrite(const uint8_t slave, const uint8_t category, const uint8_t index,
                  const uint8_t page, const uint16_t *words, const uint8_t count,
                  uint8_t *out, const size_t capacity) {
    const size_t size = 6U + 2U * static_cast<size_t>(count) + kCrcSize;
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
    return appendCrc(out, size - kCrcSize);
}

ScanResult scanResponse(const uint8_t *buffer, const size_t length, const uint8_t slave,
                        const uint8_t function, const uint8_t expectedWords, uint16_t *words,
                        const size_t wordCapacity) {
    ScanResult result;
    if (buffer == nullptr || length == 0U) return result;
    if (buffer[0] != slave) {
        result.status = ScanStatus::Skip;
        return result;
    }
    if (length < 2U) return result;

    const uint8_t replyFunction = buffer[1];
    if (replyFunction == static_cast<uint8_t>(function | 0x80U)) {
        if (length < kExceptionSize) return result;
        if (!crcValid(buffer, kExceptionSize)) {
            result.status = ScanStatus::Skip;
            result.crcMismatch = true;
            return result;
        }
        result.status = ScanStatus::Exception;
        result.frameLength = kExceptionSize;
        result.exceptionCode = buffer[2];
        return result;
    }
    if (replyFunction != function) {
        result.status = ScanStatus::Skip;
        return result;
    }
    if (length < kHeaderSize) return result;

    // Another master's request to the same slave looks like a reply with a
    // byte count taken from its category; the count checks and the CRC tell
    // the two apart.
    const uint8_t byteCount = buffer[2];
    const size_t wordCount = byteCount / 2U;
    if ((byteCount & 1U) != 0U || wordCount > kMaxRegisters
        || (expectedWords != 0U && wordCount != expectedWords)) {
        result.status = ScanStatus::Skip;
        return result;
    }
    const size_t frameLength = kHeaderSize + byteCount + kCrcSize;
    if (length < frameLength) return result;
    if (!crcValid(buffer, frameLength)) {
        result.status = ScanStatus::Skip;
        result.crcMismatch = true;
        return result;
    }

    result.status = ScanStatus::Response;
    result.frameLength = frameLength;
    result.wordCount = static_cast<uint8_t>(wordCount);
    for (size_t i = 0U; i < wordCount && words != nullptr && i < wordCapacity; ++i) {
        words[i] = static_cast<uint16_t>((buffer[kHeaderSize + 2U * i] << 8U)
                                         | buffer[kHeaderSize + 2U * i + 1U]);
    }
    return result;
}

}  // namespace WavinFrame
