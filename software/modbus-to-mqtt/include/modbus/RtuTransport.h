#ifndef MODBUS_TO_MQTT_RTU_TRANSPORT_H
#define MODBUS_TO_MQTT_RTU_TRANSPORT_H

#include <cstddef>
#include <cstdint>

#include "modbus/RtuFrame.h"

class RtuLink;

// Timing of a transaction, for the retry policy. Pure and constexpr (single
// expressions: the firmware builds as C++11) so a protocol can check its
// budget at compile time. Whole milliseconds, rounded up.
namespace RtuTiming {

// The serial formats the port accepts (kSerialFormats in Rs485Port.cpp) use
// at most 12 bits per character: 8E2 and 8O2 have a start bit, 8 data bits,
// a parity bit and 2 stop bits.
constexpr uint8_t kMaxBitsPerCharacter = 12U;

// Time `bytes` characters take on the wire at `baud`, with bitsPerCharacter
// bits per character including start, parity and stop bits (10 for 8N1, 12
// for 8E2).
constexpr uint32_t wireTimeMs(const size_t bytes, const uint32_t baud, const uint8_t bitsPerCharacter) {
    return baud == 0U
               ? 0U
               : static_cast<uint32_t>(
                   (static_cast<uint64_t>(bytes) * bitsPerCharacter * 1000U + baud - 1U) / baud);
}

// How long one attempt waits for a reply of at most replyBytes: the slave's
// turnaround plus the time the reply takes on the wire.
constexpr uint32_t attemptTimeoutMs(const uint32_t turnaroundMs, const size_t replyBytes,
                                    const uint32_t baud, const uint8_t bitsPerCharacter) {
    return turnaroundMs + wireTimeMs(replyBytes, baud, bitsPerCharacter);
}

// Whether an attempt costing attemptMs (its request on the wire plus its
// timeout) fits a transaction budget, after elapsedMs have been spent and
// with a pause of pauseMs before it.
constexpr bool attemptFits(const uint32_t elapsedMs, const uint32_t pauseMs, const uint32_t attemptMs,
                           const uint32_t budgetMs) {
    return static_cast<uint64_t>(elapsedMs) + pauseMs + attemptMs <= budgetMs;
}

}  // namespace RtuTiming

// How a protocol wants its transactions run.
struct RtuPolicy {
    // How long an attempt waits for the reply to start; the reply's own wire
    // time is added per attempt from the link's serial settings.
    uint32_t turnaroundMs;
    // Longest a transaction, retries included, may take. A retry starts only
    // if its pause and a full attempt still fit.
    uint32_t budgetMs;
    // Random pause before each retry, so it does not land in the same burst
    // of another master's traffic that swallowed the previous attempt.
    uint32_t backoffMinMs;
    uint32_t backoffMaxMs;
    // After a final timeout or CRC error, input is discarded until the line
    // has been quiet for settleQuietMs since the last byte, for at most
    // settleMaxMs, so a late reply or the tail of another master's burst is
    // not taken for the reply to the next request.
    uint32_t settleQuietMs;
    uint32_t settleMaxMs;
};

// One request and how to recognise its reply.
struct RtuRequest {
    const uint8_t *frame;
    size_t length;
    uint8_t slave;
    uint8_t function;
    // Shape of the reply, and of a request another master would send for
    // the same function (FrameShape::none() when it cannot be recognised).
    RtuFrame::FrameShape reply;
    RtuFrame::FrameShape peerRequest;
    // The longest reply the request can get, for the per-attempt timeout.
    size_t maxReplyLength;
    // Attempts while the result is a timeout or CRC error; 1 for a request
    // that must not be repeated.
    uint8_t attempts;
};

// Sends requests over an RtuLink and finds their replies among other
// traffic on the line, retrying timeouts and CRC errors within the
// policy's budget. The caller must hold the port for the whole exchange.
class RtuTransport {
public:
    RtuTransport(RtuLink &link, const RtuPolicy &policy);

    // Sends the request up to request.attempts times while the result is a
    // timeout or CRC error, pausing a random backoff in between; a retry
    // starts only if it still fits the budget. Returns a ModbusMaster-
    // compatible status. On success the reply frame, CRC included, is in
    // replyFrame and replyLength. replyFrame must hold request.reply.maxLength
    // bytes; a smaller buffer is a caller error, reported as
    // kIllegalDataValue without sending anything. After a final timeout or
    // CRC error the line is left quiet.
    uint8_t exchange(const RtuRequest &request, uint8_t *replyFrame, size_t replyCapacity,
                     size_t &replyLength);

    // Timeouts and CRC errors (typically a collision with another master);
    // exception replies from the device are final.
    static bool isRetryable(uint8_t status);

private:
    // One request and the wait for its reply, for at most timeoutMs. The
    // reply is copied to replyFrame, which exchange() has checked holds the
    // shape's largest frame. lastByteAtMs is set to the time of the last byte
    // received, or of the end of the transmission when nothing arrived.
    uint8_t attempt(const RtuRequest &request, uint32_t timeoutMs, uint8_t *replyFrame,
                    size_t &replyLength, uint32_t &lastByteAtMs);

    uint32_t drawBackoffMs();

    void settleLine(uint32_t lastByteAtMs);

    RtuLink &_link;
    RtuPolicy _policy;
};

#endif
