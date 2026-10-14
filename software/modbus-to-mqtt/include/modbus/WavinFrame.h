#ifndef MODBUS_TO_MQTT_WAVIN_FRAME_H
#define MODBUS_TO_MQTT_WAVIN_FRAME_H

#include <cstddef>
#include <cstdint>

// Frames for the Wavin AHC 9000 (also sold as Jablotron AC-116). The
// controller uses Modbus RTU framing (address, function, payload, CRC16) but
// only answers its own function codes, and addresses registers by
// category/index/page instead of a flat register number:
//
//   0x43 read:  addr 43 category index page count            crc
//   0x44 write: addr 44 category index page count value...   crc
//   reply:      addr fn byteCount data...                    crc
//   exception:  addr fn|0x80 code                            crc
//
// Pure functions without Arduino dependencies so they can be unit tested on
// the host.
namespace WavinFrame {

constexpr uint8_t kReadFunction = 0x43;
constexpr uint8_t kWriteFunction = 0x44;

// Registers per request; one byte count in the reply covers at most 127 words,
// and the controller is only known to answer requests of up to 22.
constexpr uint8_t kMaxRegisters = 32U;
constexpr size_t kReadRequestSize = 8U;
constexpr size_t kMaxWriteRequestSize = 8U + 2U * 4U;
// Largest reply: header (3) + data + CRC (2).
constexpr size_t kMaxResponseSize = 3U + 2U * kMaxRegisters + 2U;

// Status codes share ModbusMaster's numbering so callers can report them
// through the same paths: 0x00 success, 0x01..0x04 exception codes from the
// device, 0xE2 timeout, 0xE3 CRC error.
constexpr uint8_t kSuccess = 0x00U;
constexpr uint8_t kResponseTimedOut = 0xE2U;
constexpr uint8_t kInvalidCrc = 0xE3U;

uint16_t crc16(const uint8_t *data, size_t length);

// Writes the request into out and returns its size, or 0 when the arguments
// do not fit (count 0, count above kMaxRegisters, or out too small).
size_t buildRead(uint8_t slave, uint8_t category, uint8_t index, uint8_t page, uint8_t count,
                 uint8_t *out, size_t capacity);
size_t buildWrite(uint8_t slave, uint8_t category, uint8_t index, uint8_t page,
                  const uint16_t *words, uint8_t count, uint8_t *out, size_t capacity);

enum class ScanStatus : uint8_t {
    // Not enough bytes yet for a complete frame.
    Incomplete = 0,
    // A complete, CRC-valid reply to the request; words were filled.
    Response,
    // A complete, CRC-valid exception reply; exceptionCode was filled.
    Exception,
    // The first byte cannot start the expected reply (another master's
    // traffic, line noise or a CRC mismatch): drop it and scan again.
    Skip,
};

struct ScanResult {
    ScanStatus status{ScanStatus::Incomplete};
    // Size of the frame at the start of the buffer (Response/Exception).
    size_t frameLength{0U};
    uint8_t exceptionCode{0U};
    uint8_t wordCount{0U};
    // Set on Skip when a frame-shaped candidate failed its CRC.
    bool crcMismatch{false};
};

// Scans the bytes received so far for the reply to a request with the given
// slave and function. expectedWords is the register count of a read (the reply
// must carry exactly that many words) or 0 to accept any count (writes).
// Up to wordCapacity big-endian words are copied into words.
ScanResult scanResponse(const uint8_t *buffer, size_t length, uint8_t slave, uint8_t function,
                        uint8_t expectedWords, uint16_t *words, size_t wordCapacity);

}  // namespace WavinFrame

#endif
