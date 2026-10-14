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

// Registers per request: Wavin's Modbus description for the AHC 9000
// (2013-08-14, sections 4.1/4.3) allows N = 0x01..0x16 for 0x43 and 0x44.
// The controller answers some requests with fewer registers than asked for;
// such replies are accepted and their actual count reported.
constexpr uint8_t kMaxRegisters = 22U;
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
    uint8_t wordCount{0U};
    // Set on Skip when a frame-shaped candidate failed its CRC and is not
    // another master's request.
    bool crcMismatch{false};
};

// Scans the bytes received so far for the reply to a request with the given
// slave and function. expectedWords is the register count of a read: the
// reply carries at most that many words, fewer when the controller does not
// answer the whole request. 0 accepts any count (writes). Up to wordCapacity
// big-endian words are copied into words.
//
// Another master's request to the same slave starts with the same two bytes
// and reads as a reply whose byte count is its category; its own CRC tells
// the two apart once enough of it has arrived. Until then a candidate that
// failed its reply CRC stays Incomplete, unless final says no more bytes
// will come: it is then a corrupted frame and reported as a CRC mismatch.
ScanResult scanResponse(const uint8_t *buffer, size_t length, uint8_t slave, uint8_t function,
                        uint8_t expectedWords, uint16_t *words, size_t wordCapacity,
                        bool final = false);

struct LocateResult {
    // Response, Exception or Incomplete; never Skip.
    ScanStatus status{ScanStatus::Incomplete};
    // Leading bytes that can no longer start the reply; the caller drops
    // them before adding more bytes.
    size_t dropCount{0U};
    uint8_t exceptionCode{0U};
    uint8_t wordCount{0U};
    // A candidate at the head of the buffer failed its CRC.
    bool crcMismatch{false};
};

// Locates the reply in the bytes received so far. Bytes at the head that
// cannot start it are counted in dropCount. A candidate at the head that is
// still incomplete may claim more bytes than will ever arrive (stale bytes,
// or another master's frame), so every later offset is checked as well and
// a complete reply behind such a candidate is still found.
LocateResult locateResponse(const uint8_t *buffer, size_t length, uint8_t slave, uint8_t function,
                            uint8_t expectedWords, uint16_t *words, size_t wordCapacity,
                            bool final = false);

}  // namespace WavinFrame

#endif
