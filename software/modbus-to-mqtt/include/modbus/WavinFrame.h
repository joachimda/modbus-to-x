#ifndef MODBUS_TO_MQTT_WAVIN_FRAME_H
#define MODBUS_TO_MQTT_WAVIN_FRAME_H

#include <cstddef>
#include <cstdint>

#include "modbus/RtuFrame.h"

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
// Request building, the frame shapes the RTU scanner needs to find a reply,
// and reply decoding. Pure functions without Arduino dependencies so they
// can be unit tested on the host.
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
// Offset of the register count in a request; a write request's size follows
// from it.
constexpr size_t kRequestCountOffset = 5U;
// A reply's header: address, function, byte count.
constexpr size_t kReplyHeaderSize = 3U;

// Size of a reply carrying `words` registers: header, data and CRC.
constexpr size_t responseSize(const uint8_t words) {
    return kReplyHeaderSize + 2U * static_cast<size_t>(words) + RtuFrame::kCrcSize;
}

// Largest reply.
constexpr size_t kMaxResponseSize = responseSize(kMaxRegisters);

// Writes the request into out and returns its size, or 0 when the arguments
// do not fit (count 0, count above kMaxRegisters, or out too small).
size_t buildRead(uint8_t slave, uint8_t category, uint8_t index, uint8_t page, uint8_t count,
                 uint8_t *out, size_t capacity);
size_t buildWrite(uint8_t slave, uint8_t category, uint8_t index, uint8_t page,
                  const uint16_t *words, uint8_t count, uint8_t *out, size_t capacity);

// Shape of the reply to a request for `count` registers: the byte count at
// offset 2 is even and at most 2 x count, since the controller answers with
// fewer registers than requested but never with more (a longer frame is
// another master's traffic). 0 accepts any count up to kMaxRegisters, for
// writes, whose reply echoes the written data.
constexpr RtuFrame::FrameShape replyShape(const uint8_t count) {
    return RtuFrame::FrameShape::counted(
        2U, 1U, RtuFrame::kCrcSize,
        responseSize(count == 0U || count > kMaxRegisters ? kMaxRegisters : count), 2U);
}

// Shape of a request another master (the wall display) sends for the same
// function, so the scanner can tell it from a reply to ours once its CRC
// has arrived: a read is kReadRequestSize bytes, a write carries its
// register count (1..4 words) at kRequestCountOffset.
constexpr RtuFrame::FrameShape peerRequestShape(const uint8_t function) {
    return function == kReadFunction
               ? RtuFrame::FrameShape::fixed(kReadRequestSize)
               : function == kWriteFunction
                     ? RtuFrame::FrameShape::counted(kRequestCountOffset, 2U, RtuFrame::kCrcSize,
                                                     kMaxWriteRequestSize, 1U, 1U)
                     : RtuFrame::FrameShape::none();
}

// Decodes the registers of a complete reply frame (big-endian words) into
// up to wordCapacity words and returns how many the frame carries, which
// may be more than were copied. 0 for a frame too short to be a reply.
uint8_t decodeReply(const uint8_t *frame, size_t length, uint16_t *words, size_t wordCapacity);

}  // namespace WavinFrame

#endif
