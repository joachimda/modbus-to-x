#ifndef MODBUS_TO_MQTT_RTU_FRAME_H
#define MODBUS_TO_MQTT_RTU_FRAME_H

#include <cstddef>
#include <cstdint>

// Modbus RTU framing shared by every protocol on the bus: the CRC16 and the
// scanner that finds one slave's reply in a byte stream that may also carry
// another master's traffic. The protocol describes its frames as shapes
// (how a frame's length follows from its first bytes); the scanner knows
// nothing else about them. Pure functions without Arduino dependencies so
// they can be unit tested on the host.
namespace RtuFrame {

// Status codes share ModbusMaster's numbering so callers can report them
// through the same paths: 0x00 success, 0x01..0x04 exception codes from the
// device, 0xE2 timeout, 0xE3 CRC error.
constexpr uint8_t kSuccess = 0x00U;
constexpr uint8_t kIllegalDataAddress = 0x02U;
constexpr uint8_t kIllegalDataValue = 0x03U;
constexpr uint8_t kResponseTimedOut = 0xE2U;
constexpr uint8_t kInvalidCrc = 0xE3U;

constexpr size_t kCrcSize = 2U;
// An exception reply: address, function | 0x80, code, CRC.
constexpr size_t kExceptionSize = 5U;
// The longest RTU frame: address, function, 252 data bytes, CRC.
constexpr size_t kMaxFrameSize = 256U;

uint16_t crc16(const uint8_t *data, size_t length);

// Appends the CRC of frame[0..length) and returns the new length.
size_t appendCrc(uint8_t *frame, size_t length);

// A frame's CRC over all of its bytes, CRC included, is zero when intact.
bool crcValid(const uint8_t *frame, size_t length);

// How a frame's length follows from its first bytes.
struct FrameShape {
    enum class Kind : uint8_t {
        // No frame of this kind exists (for example when another master's
        // requests cannot be recognised).
        None = 0,
        // Always fixedLength bytes.
        Fixed,
        // The byte at countOffset counts the countUnit-byte items that follow
        // it, then trailer bytes (the CRC) end the frame:
        // length = countOffset + 1 + count * countUnit + trailer.
        Counted,
    };

    Kind kind;
    size_t fixedLength;
    size_t countOffset;
    size_t countUnit;
    size_t trailer;
    // A count that is not a multiple of countMultiple or below minCount, or
    // a frame longer than maxLength, is not of this shape (another master's
    // traffic rather than the reply).
    size_t countMultiple;
    size_t minCount;
    size_t maxLength;

    constexpr FrameShape()
        : kind(Kind::None), fixedLength(0U), countOffset(0U), countUnit(0U), trailer(0U),
          countMultiple(1U), minCount(0U), maxLength(0U) {
    }

    constexpr FrameShape(const Kind kindValue, const size_t fixedLengthValue,
                         const size_t countOffsetValue, const size_t countUnitValue,
                         const size_t trailerValue, const size_t countMultipleValue,
                         const size_t minCountValue, const size_t maxLengthValue)
        : kind(kindValue), fixedLength(fixedLengthValue), countOffset(countOffsetValue),
          countUnit(countUnitValue), trailer(trailerValue), countMultiple(countMultipleValue),
          minCount(minCountValue), maxLength(maxLengthValue) {
    }

    static constexpr FrameShape none() {
        return FrameShape();
    }

    static constexpr FrameShape fixed(const size_t length) {
        return FrameShape(Kind::Fixed, length, 0U, 0U, 0U, 1U, 0U, length);
    }

    static constexpr FrameShape counted(const size_t countOffsetValue, const size_t countUnitValue,
                                        const size_t trailerValue, const size_t maxLengthValue,
                                        const size_t countMultipleValue = 1U,
                                        const size_t minCountValue = 0U) {
        return FrameShape(Kind::Counted, 0U, countOffsetValue, countUnitValue, trailerValue,
                          countMultipleValue, minCountValue, maxLengthValue);
    }
};

enum class ScanStatus : uint8_t {
    // Not enough bytes yet for a complete frame.
    Incomplete = 0,
    // A complete, CRC-valid reply to the request.
    Response,
    // A complete, CRC-valid exception reply; exceptionCode was filled.
    Exception,
    // The bytes at the start of the buffer cannot start the expected reply
    // (another master's traffic, line noise or a CRC mismatch): drop
    // frameLength of them and scan again.
    Skip,
};

struct ScanResult {
    ScanStatus status{ScanStatus::Incomplete};
    // Size of the frame at the start of the buffer: the reply (Response,
    // Exception) or the bytes to drop (Skip: 1, or a whole request from
    // another master once it was recognised).
    size_t frameLength{0U};
    uint8_t exceptionCode{0U};
    // Set on Skip when a frame-shaped candidate failed its CRC and is not
    // another master's request. The candidate may be the reply, corrupted,
    // or a look-alike inside another frame's data (the slave address and
    // function happening to occur as data bytes); the two cannot be told
    // apart, so a mismatch means a corrupted or look-alike frame was seen,
    // not that the reply itself was.
    bool crcMismatch{false};
};

// Scans the bytes received so far for the reply to a request with the given
// slave and function: a frame of the reply shape, or the exception form
// (function | 0x80, one code byte).
//
// Another master's request to the same slave starts with the same two bytes
// and may read as a reply; its own CRC tells the two apart once enough of
// it has arrived, if peerRequest describes its shape. Until then a candidate
// that failed its reply CRC stays Incomplete, unless final says no more
// bytes will come: it is then a corrupted frame and reported as a CRC
// mismatch.
ScanResult scan(const uint8_t *buffer, size_t length, uint8_t slave, uint8_t function,
                const FrameShape &reply, const FrameShape &peerRequest, bool final = false);

struct LocateResult {
    // Response, Exception or Incomplete; never Skip.
    ScanStatus status{ScanStatus::Incomplete};
    // Leading bytes that can no longer start the reply; the caller drops
    // them before adding more bytes.
    size_t dropCount{0U};
    // Where the complete reply starts and how long it is (Response and
    // Exception).
    size_t frameOffset{0U};
    size_t frameLength{0U};
    uint8_t exceptionCode{0U};
    // A candidate at the head of the buffer failed its CRC.
    bool crcMismatch{false};
};

// Locates the reply in the bytes received so far. Bytes at the head that
// cannot start it are counted in dropCount. A candidate at the head that is
// still incomplete may claim more bytes than will ever arrive (stale bytes,
// or another master's frame), so every later offset is checked as well and
// a complete reply behind such a candidate is still found.
LocateResult locate(const uint8_t *buffer, size_t length, uint8_t slave, uint8_t function,
                    const FrameShape &reply, const FrameShape &peerRequest, bool final = false);

}  // namespace RtuFrame

#endif
