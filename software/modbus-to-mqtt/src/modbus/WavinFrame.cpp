#include "modbus/WavinFrame.h"

namespace WavinFrame {
namespace {

constexpr size_t kHeaderSize = 3U;
constexpr size_t kCrcSize = 2U;
constexpr size_t kExceptionSize = 5U;
// Offset of the register count in a request; a write request's size follows
// from it.
constexpr size_t kRequestCountOffset = 5U;

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

enum class RequestMatch : uint8_t {
    // The bytes cannot be a request of this function.
    None = 0,
    // Not enough bytes yet to check the request's CRC.
    Undecided,
    // A complete, CRC-valid request from another master; size was set.
    Request,
};

// Whether the bytes are a request of the given function, as another master
// on the bus would send it to the same slave.
RequestMatch matchRequest(const uint8_t *buffer, const size_t length, const uint8_t function,
                          size_t &size) {
    if (function == kReadFunction) {
        size = kReadRequestSize;
    } else if (function == kWriteFunction) {
        if (length <= kRequestCountOffset) return RequestMatch::Undecided;
        const uint8_t count = buffer[kRequestCountOffset];
        size = kRequestCountOffset + 1U + 2U * static_cast<size_t>(count) + kCrcSize;
        if (count == 0U || size > kMaxWriteRequestSize) return RequestMatch::None;
    } else {
        return RequestMatch::None;
    }
    if (length < size) return RequestMatch::Undecided;
    return crcValid(buffer, size) ? RequestMatch::Request : RequestMatch::None;
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

ScanResult scanResponse(const uint8_t *buffer, const size_t length, const uint8_t slave,
                        const uint8_t function, const uint8_t expectedWords, uint16_t *words,
                        const size_t wordCapacity, const bool final) {
    ScanResult result;
    if (buffer == nullptr || length == 0U) return result;
    if (buffer[0] != slave) {
        result.status = ScanStatus::Skip;
        result.frameLength = 1U;
        return result;
    }
    if (length < 2U) return result;

    const uint8_t replyFunction = buffer[1];
    if (replyFunction == static_cast<uint8_t>(function | 0x80U)) {
        if (length < kExceptionSize) return result;
        if (!crcValid(buffer, kExceptionSize)) {
            result.status = ScanStatus::Skip;
            result.frameLength = 1U;
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
        result.frameLength = 1U;
        return result;
    }
    if (length < kHeaderSize) return result;

    // The controller may answer with fewer registers than requested, never
    // with more: a longer frame is another master's traffic.
    const uint8_t byteCount = buffer[2];
    const size_t wordCount = byteCount / 2U;
    if ((byteCount & 1U) != 0U || wordCount > kMaxRegisters
        || (expectedWords != 0U && wordCount > expectedWords)) {
        result.status = ScanStatus::Skip;
        result.frameLength = 1U;
        return result;
    }
    const size_t frameLength = kHeaderSize + byteCount + kCrcSize;
    if (length < frameLength) return result;
    if (!crcValid(buffer, frameLength)) {
        // Another master's request to the same slave starts with the same
        // two bytes and reads as a reply whose byte count is its category.
        // Its own CRC tells it apart, once all of it has arrived.
        size_t requestSize = 0U;
        switch (matchRequest(buffer, length, function, requestSize)) {
            case RequestMatch::Undecided:
                if (!final) return result;
                break;
            case RequestMatch::Request:
                result.status = ScanStatus::Skip;
                result.frameLength = requestSize;
                return result;
            case RequestMatch::None:
                break;
        }
        result.status = ScanStatus::Skip;
        result.frameLength = 1U;
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

LocateResult locateResponse(const uint8_t *buffer, const size_t length, const uint8_t slave,
                            const uint8_t function, const uint8_t expectedWords, uint16_t *words,
                            const size_t wordCapacity, const bool final) {
    LocateResult result;
    if (buffer == nullptr) return result;
    const auto found = [&result](const ScanResult &scan) {
        result.status = scan.status;
        result.exceptionCode = scan.exceptionCode;
        result.wordCount = scan.wordCount;
    };

    // Drop bytes at the head that can never start the reply; only they are
    // allowed to report a CRC mismatch, bytes inside a frame are not frames.
    size_t head = 0U;
    while (head < length) {
        const ScanResult scan = scanResponse(buffer + head, length - head, slave, function,
                                             expectedWords, words, wordCapacity, final);
        if (scan.status == ScanStatus::Skip) {
            result.crcMismatch = result.crcMismatch || scan.crcMismatch;
            head += scan.frameLength > 0U ? scan.frameLength : 1U;
            continue;
        }
        if (scan.status != ScanStatus::Incomplete) {
            found(scan);
            result.dropCount = head;
            return result;
        }
        break;
    }
    result.dropCount = head;

    // The head is an incomplete candidate. It may claim more bytes than will
    // ever arrive, so the reply is also looked for behind it.
    for (size_t offset = head + 1U; offset < length; ++offset) {
        const ScanResult scan = scanResponse(buffer + offset, length - offset, slave, function,
                                             expectedWords, words, wordCapacity, final);
        if (scan.status == ScanStatus::Response || scan.status == ScanStatus::Exception) {
            found(scan);
            return result;
        }
    }
    return result;
}

}  // namespace WavinFrame
