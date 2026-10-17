#include "modbus/RtuTransport.h"

#include <cstring>

#include "modbus/RtuLink.h"

RtuTransport::RtuTransport(RtuLink &link, const RtuPolicy &policy) : _link(link), _policy(policy) {
}

uint8_t RtuTransport::exchange(const RtuRequest &request, uint8_t *replyFrame,
                               const size_t replyCapacity, size_t &replyLength) {
    replyLength = 0U;
    // A buffer that cannot hold the largest reply of the shape is a caller
    // error; a cut reply must never pass as a complete one.
    if (replyFrame == nullptr || replyCapacity < request.reply.maxLength) {
        return RtuFrame::kIllegalDataValue;
    }
    // An attempt waits for the slave's turnaround plus the time the largest
    // reply takes on the wire. What it costs the budget also counts the
    // request's own wire time.
    const uint32_t baud = _link.baudRate();
    const uint8_t bitsPerCharacter = _link.bitsPerCharacter();
    const uint32_t timeoutMs = RtuTiming::attemptTimeoutMs(_policy.turnaroundMs, request.maxReplyLength,
                                                           baud, bitsPerCharacter);
    const uint32_t attemptMs = RtuTiming::wireTimeMs(request.length, baud, bitsPerCharacter) + timeoutMs;
    const uint32_t startedAt = _link.nowMs();
    const uint8_t attempts = request.attempts > 0U ? request.attempts : 1U;
    for (uint8_t attemptNumber = 1U;; ++attemptNumber) {
        uint32_t lastByteAt = 0U;
        const uint8_t status = attempt(request, timeoutMs, replyFrame, replyLength, lastByteAt);
        if (!isRetryable(status)) return status;
        if (attemptNumber < attempts) {
            // The pause and a full attempt must still fit the transaction
            // budget; otherwise this failure is final.
            const uint32_t pauseMs = drawBackoffMs();
            if (RtuTiming::attemptFits(_link.nowMs() - startedAt, pauseMs, attemptMs, _policy.budgetMs)) {
                _link.idleMs(pauseMs);
                continue;
            }
        }
        // The last attempt failed. A reply still on its way, or the tail of
        // the burst that swallowed the attempt, must not be taken for the
        // reply to the next request. A late reply to an earlier attempt that
        // lands during a retry answers the same request and is harmless, so
        // only the final failure waits for the line to go quiet.
        settleLine(lastByteAt);
        return status;
    }
}

bool RtuTransport::isRetryable(const uint8_t status) {
    return status == RtuFrame::kResponseTimedOut || status == RtuFrame::kInvalidCrc;
}

uint8_t RtuTransport::attempt(const RtuRequest &request, const uint32_t timeoutMs, uint8_t *replyFrame,
                              size_t &replyLength, uint32_t &lastByteAtMs) {
    // Bytes left over from another master's traffic would be mistaken for
    // the start of the reply.
    _link.discardInput();
    _link.send(request.frame, request.length);

    // Sized for the longest RTU frame so any profile's reply shape fits; a
    // Wavin reply is at most 49 bytes.
    uint8_t buffer[RtuFrame::kMaxFrameSize];
    size_t received = 0U;
    bool crcMismatch = false;
    const uint32_t startedAt = _link.nowMs();
    lastByteAtMs = startedAt;
    while (_link.nowMs() - startedAt < timeoutMs) {
        const int value = _link.read();
        if (value < 0) {
            _link.idleMs(1U);
            continue;
        }
        lastByteAtMs = _link.nowMs();
        if (received == sizeof buffer) {
            std::memmove(buffer, buffer + 1, received - 1U);
            --received;
        }
        buffer[received++] = static_cast<uint8_t>(value);

        // The reply is looked for at every offset, so bytes ahead of it
        // (another master's traffic, or a candidate claiming a longer frame
        // than will arrive) cannot hide it; those that can no longer start
        // it are dropped.
        const RtuFrame::LocateResult found = RtuFrame::locate(buffer, received, request.slave,
                                                              request.function, request.reply,
                                                              request.peerRequest);
        crcMismatch = crcMismatch || found.crcMismatch;
        if (found.status == RtuFrame::ScanStatus::Response) {
            // The shape bounds the frame by the capacity exchange() required.
            replyLength = found.frameLength;
            std::memcpy(replyFrame, buffer + found.frameOffset, replyLength);
            return RtuFrame::kSuccess;
        }
        if (found.status == RtuFrame::ScanStatus::Exception) return found.exceptionCode;
        if (found.dropCount > 0U) {
            std::memmove(buffer, buffer + found.dropCount, received - found.dropCount);
            received -= found.dropCount;
        }
    }
    // No more bytes will come: a candidate that failed its CRC but could
    // still have been the start of another master's request is now known to
    // be a corrupted frame.
    crcMismatch = crcMismatch
                  || RtuFrame::locate(buffer, received, request.slave, request.function, request.reply,
                                      request.peerRequest, true).crcMismatch;
    return crcMismatch ? RtuFrame::kInvalidCrc : RtuFrame::kResponseTimedOut;
}

uint32_t RtuTransport::drawBackoffMs() {
    if (_policy.backoffMaxMs <= _policy.backoffMinMs) return _policy.backoffMinMs;
    const uint32_t span = _policy.backoffMaxMs - _policy.backoffMinMs + 1U;
    return _policy.backoffMinMs + _link.randomWord() % span;
}

void RtuTransport::settleLine(const uint32_t lastByteAtMs) {
    const uint32_t startedAt = _link.nowMs();
    uint32_t quietSince = lastByteAtMs;
    // Measured from the attempt's last byte: after a timeout in which nothing
    // arrived the line is already quiet and this returns at once. The cap
    // keeps a talker that never stops from holding the caller.
    while (_link.nowMs() - quietSince < _policy.settleQuietMs
           && _link.nowMs() - startedAt < _policy.settleMaxMs) {
        if (_link.read() >= 0) {
            quietSince = _link.nowMs();
            continue;
        }
        _link.idleMs(1U);
    }
}
