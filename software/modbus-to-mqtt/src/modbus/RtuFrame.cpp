#include "modbus/RtuFrame.h"

namespace RtuFrame {
namespace {

// Length of a frame of the shape, from its first bytes. Returns 0 when the
// bytes cannot be such a frame, and sets undecided when more bytes are
// needed to tell.
size_t frameLength(const FrameShape &shape, const uint8_t *buffer, const size_t length,
                   bool &undecided) {
    undecided = false;
    switch (shape.kind) {
        case FrameShape::Kind::Fixed:
            return shape.fixedLength;
        case FrameShape::Kind::Counted: {
            if (length <= shape.countOffset) {
                undecided = true;
                return 0U;
            }
            const size_t count = buffer[shape.countOffset];
            if (count < shape.minCount || (count % shape.countMultiple) != 0U) return 0U;
            const size_t size = shape.countOffset + 1U + count * shape.countUnit + shape.trailer;
            return size <= shape.maxLength ? size : 0U;
        }
        case FrameShape::Kind::None:
            break;
    }
    return 0U;
}

enum class RequestMatch : uint8_t {
    // The bytes cannot be a request of this shape.
    None = 0,
    // Not enough bytes yet to check the request's CRC.
    Undecided,
    // A complete, CRC-valid request from another master; size was set.
    Request,
};

// Whether the bytes are a request as another master on the bus would send
// it to the same slave with the same function.
RequestMatch matchRequest(const FrameShape &peerRequest, const uint8_t *buffer, const size_t length,
                          size_t &size) {
    if (peerRequest.kind == FrameShape::Kind::None) return RequestMatch::None;
    bool undecided = false;
    size = frameLength(peerRequest, buffer, length, undecided);
    if (undecided) return RequestMatch::Undecided;
    if (size == 0U) return RequestMatch::None;
    if (length < size) return RequestMatch::Undecided;
    return crcValid(buffer, size) ? RequestMatch::Request : RequestMatch::None;
}

ScanResult skipOne(const bool crcMismatch) {
    ScanResult result;
    result.status = ScanStatus::Skip;
    result.frameLength = 1U;
    result.crcMismatch = crcMismatch;
    return result;
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

size_t appendCrc(uint8_t *frame, const size_t length) {
    const uint16_t crc = crc16(frame, length);
    frame[length] = static_cast<uint8_t>(crc & 0xFFU);
    frame[length + 1U] = static_cast<uint8_t>(crc >> 8U);
    return length + kCrcSize;
}

bool crcValid(const uint8_t *frame, const size_t length) {
    return crc16(frame, length) == 0U;
}

ScanResult scan(const uint8_t *buffer, const size_t length, const uint8_t slave,
                const uint8_t function, const FrameShape &reply, const FrameShape &peerRequest,
                const bool final) {
    ScanResult result;
    if (buffer == nullptr || length == 0U) return result;
    if (buffer[0] != slave) return skipOne(false);
    if (length < 2U) return result;

    const uint8_t replyFunction = buffer[1];
    if (replyFunction == static_cast<uint8_t>(function | 0x80U)) {
        if (length < kExceptionSize) return result;
        if (!crcValid(buffer, kExceptionSize)) return skipOne(true);
        result.status = ScanStatus::Exception;
        result.frameLength = kExceptionSize;
        result.exceptionCode = buffer[2];
        return result;
    }
    if (replyFunction != function) return skipOne(false);

    // A frame outside the reply's shape (for Wavin: more registers than
    // requested) is another master's traffic, not a reply.
    bool undecided = false;
    const size_t size = frameLength(reply, buffer, length, undecided);
    if (undecided) return result;
    if (size == 0U) return skipOne(false);
    if (length < size) return result;
    if (!crcValid(buffer, size)) {
        // Another master's request to the same slave starts with the same
        // two bytes and may read as a reply. Its own CRC tells it apart, once
        // all of it has arrived.
        size_t requestSize = 0U;
        switch (matchRequest(peerRequest, buffer, length, requestSize)) {
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
        return skipOne(true);
    }

    result.status = ScanStatus::Response;
    result.frameLength = size;
    return result;
}

LocateResult locate(const uint8_t *buffer, const size_t length, const uint8_t slave,
                    const uint8_t function, const FrameShape &reply, const FrameShape &peerRequest,
                    const bool final) {
    LocateResult result;
    if (buffer == nullptr) return result;
    const auto found = [&result](const ScanResult &scanned, const size_t offset) {
        result.status = scanned.status;
        result.exceptionCode = scanned.exceptionCode;
        result.frameOffset = offset;
        result.frameLength = scanned.frameLength;
    };

    // Drop bytes at the head that can never start the reply; only they are
    // allowed to report a CRC mismatch, bytes inside a frame are not frames.
    size_t head = 0U;
    while (head < length) {
        const ScanResult scanned = scan(buffer + head, length - head, slave, function, reply,
                                        peerRequest, final);
        if (scanned.status == ScanStatus::Skip) {
            result.crcMismatch = result.crcMismatch || scanned.crcMismatch;
            head += scanned.frameLength > 0U ? scanned.frameLength : 1U;
            continue;
        }
        if (scanned.status != ScanStatus::Incomplete) {
            found(scanned, head);
            result.dropCount = head;
            return result;
        }
        break;
    }
    result.dropCount = head;

    // The head is an incomplete candidate. It may claim more bytes than will
    // ever arrive, so the reply is also looked for behind it.
    for (size_t offset = head + 1U; offset < length; ++offset) {
        const ScanResult scanned = scan(buffer + offset, length - offset, slave, function, reply,
                                        peerRequest, final);
        if (scanned.status == ScanStatus::Response || scanned.status == ScanStatus::Exception) {
            found(scanned, offset);
            return result;
        }
    }
    return result;
}

}  // namespace RtuFrame
