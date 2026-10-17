#include "modbus/WavinClient.h"

#include "Config.h"
#include "modbus/RtuFrame.h"
#include "modbus/RtuTransport.h"
#include "modbus/WavinFrame.h"

namespace {

constexpr RtuPolicy kPolicy = {WAVIN_TURNAROUND_MS,        WAVIN_TRANSACTION_BUDGET_MS,
                               WAVIN_RETRY_BACKOFF_MIN_MS, WAVIN_RETRY_BACKOFF_MAX_MS,
                               WAVIN_SETTLE_QUIET_MS,      WAVIN_SETTLE_MAX_MS};

static_assert(WAVIN_READ_ATTEMPTS >= 1U && WAVIN_WRITE_ATTEMPTS >= 1U, "at least one attempt");
static_assert(WAVIN_RETRY_BACKOFF_MIN_MS <= WAVIN_RETRY_BACKOFF_MAX_MS, "backoff range");
static_assert(WAVIN_SETTLE_QUIET_MS <= WAVIN_SETTLE_MAX_MS, "settle range");
// The first attempt is always made, so by itself it must fit the transaction
// budget even for the largest reply at the lowest baud rate the bus
// configuration allows; otherwise a datapoint could become unreadable. Later
// attempts are started only if they fit (RtuTransport).
static_assert(RtuTiming::attemptFits(
                  0U, 0U,
                  RtuTiming::wireTimeMs(WavinFrame::kMaxWriteRequestSize, MODBUS_MIN_BAUD_RATE,
                                        RtuTiming::kMaxBitsPerCharacter)
                      + RtuTiming::attemptTimeoutMs(WAVIN_TURNAROUND_MS, WavinFrame::kMaxResponseSize,
                                                    MODBUS_MIN_BAUD_RATE,
                                                    RtuTiming::kMaxBitsPerCharacter),
                  WAVIN_TRANSACTION_BUDGET_MS),
              "one Wavin attempt with the largest reply at the lowest baud rate must fit the "
              "transaction budget");

}  // namespace

WavinClient::WavinClient(RtuLink &link) : _link(link) {
}

uint8_t WavinClient::readRegisters(const uint8_t slave, const uint8_t category, const uint8_t index,
                                   const uint8_t page, const uint8_t count, uint16_t *words,
                                   const size_t wordCapacity, uint8_t &receivedCount) {
    receivedCount = 0U;
    uint8_t request[WavinFrame::kReadRequestSize];
    const size_t length = WavinFrame::buildRead(slave, category, index, page, count, request,
                                                sizeof request);
    if (length == 0U) return RtuFrame::kIllegalDataValue;
    // A wall display polling the same bus occasionally collides with a
    // request. Reads have no side effects, so they are retried.
    return exchange(request, length, slave, WavinFrame::kReadFunction, count, WAVIN_READ_ATTEMPTS,
                    words, wordCapacity, receivedCount);
}

uint8_t WavinClient::writeRegisters(const uint8_t slave, const uint8_t category, const uint8_t index,
                                    const uint8_t page, const uint16_t *words, const uint8_t count) {
    uint8_t request[WavinFrame::kMaxWriteRequestSize];
    const size_t length = WavinFrame::buildWrite(slave, category, index, page, words, count, request,
                                                 sizeof request);
    if (length == 0U) return RtuFrame::kIllegalDataValue;
    // 0x44 sets registers to absolute values, so repeating a write whose reply
    // was lost to a collision leaves the same result: it is retried like a
    // read. The reply echoes the written data, but any word count up to
    // kMaxRegisters is accepted, so the timeout allows for the largest.
    uint8_t ignored = 0U;
    return exchange(request, length, slave, WavinFrame::kWriteFunction, WavinFrame::kMaxRegisters,
                    WAVIN_WRITE_ATTEMPTS, nullptr, 0U, ignored);
}

uint8_t WavinClient::exchange(const uint8_t *request, const size_t requestLength, const uint8_t slave,
                              const uint8_t function, const uint8_t replyWords, const uint8_t attempts,
                              uint16_t *words, const size_t wordCapacity, uint8_t &receivedCount) {
    receivedCount = 0U;
    RtuRequest rtu{};
    rtu.frame = request;
    rtu.length = requestLength;
    rtu.slave = slave;
    rtu.function = function;
    rtu.reply = WavinFrame::replyShape(replyWords);
    rtu.peerRequest = WavinFrame::peerRequestShape(function);
    rtu.maxReplyLength = rtu.reply.maxLength;
    rtu.attempts = attempts;

    uint8_t frame[WavinFrame::kMaxResponseSize];
    size_t length = 0U;
    const uint8_t status = RtuTransport(_link, kPolicy).exchange(rtu, frame, sizeof frame, length);
    if (status == RtuFrame::kSuccess) {
        receivedCount = WavinFrame::decodeReply(frame, length, words, wordCapacity);
    }
    return status;
}
