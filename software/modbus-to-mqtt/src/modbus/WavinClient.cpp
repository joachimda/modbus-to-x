#include "modbus/WavinClient.h"

#include <Arduino.h>
#include <cstring>
#include <esp_random.h>

#include "Config.h"
#include "ModbusMaster.h"
#include "modbus/ModbusBus.h"
#include "modbus/WavinFrame.h"

static_assert(WAVIN_READ_ATTEMPTS >= 1U && WAVIN_WRITE_ATTEMPTS >= 1U, "at least one attempt");
static_assert(WAVIN_RETRY_BACKOFF_MIN_MS <= WAVIN_RETRY_BACKOFF_MAX_MS, "backoff range");
// The first attempt is always made, so by itself it must fit the transaction
// budget even for the largest reply at the lowest baud rate the bus
// configuration allows; otherwise a datapoint could become unreadable. Later
// attempts are started only if they fit (transactWithRetry).
static_assert(WavinFrame::attemptFits(
                  0U, 0U,
                  WavinFrame::wireTimeMs(WavinFrame::kMaxWriteRequestSize, MODBUS_MIN_BAUD_RATE,
                                         WavinFrame::kMaxBitsPerCharacter)
                      + WavinFrame::attemptTimeoutMs(WAVIN_TURNAROUND_MS, WavinFrame::kMaxRegisters,
                                                     MODBUS_MIN_BAUD_RATE,
                                                     WavinFrame::kMaxBitsPerCharacter),
                  WAVIN_TRANSACTION_BUDGET_MS),
              "one Wavin attempt with the largest reply at the lowest baud rate must fit the "
              "transaction budget");

WavinClient::WavinClient(ModbusBus &bus) : _bus(bus) {
}

uint8_t WavinClient::readRegisters(const uint8_t slave, const uint8_t category, const uint8_t index,
                                   const uint8_t page, const uint8_t count, uint16_t *words,
                                   const size_t wordCapacity, uint8_t &receivedCount) {
    receivedCount = 0U;
    uint8_t request[WavinFrame::kReadRequestSize];
    const size_t length = WavinFrame::buildRead(slave, category, index, page, count, request,
                                                sizeof request);
    if (length == 0U) return ModbusMaster::ku8MBIllegalDataValue;
    // A wall display polling the same bus occasionally collides with a
    // request. Reads have no side effects, so they are retried.
    return transactWithRetry(WAVIN_READ_ATTEMPTS, request, length, slave, WavinFrame::kReadFunction,
                             count, count, words, wordCapacity, receivedCount);
}

uint8_t WavinClient::writeRegisters(const uint8_t slave, const uint8_t category, const uint8_t index,
                                    const uint8_t page, const uint16_t *words, const uint8_t count) {
    uint8_t request[WavinFrame::kMaxWriteRequestSize];
    const size_t length = WavinFrame::buildWrite(slave, category, index, page, words, count, request,
                                                 sizeof request);
    if (length == 0U) return ModbusMaster::ku8MBIllegalDataValue;
    // 0x44 sets registers to absolute values, so repeating a write whose reply
    // was lost to a collision leaves the same result: it is retried like a
    // read. The reply echoes the written data, but any word count up to
    // kMaxRegisters is accepted, so the timeout allows for the largest.
    uint8_t ignored = 0U;
    return transactWithRetry(WAVIN_WRITE_ATTEMPTS, request, length, slave, WavinFrame::kWriteFunction,
                             0U, WavinFrame::kMaxRegisters, nullptr, 0U, ignored);
}

uint8_t WavinClient::transactWithRetry(const uint8_t attempts, const uint8_t *request,
                                       const size_t requestLength, const uint8_t slave,
                                       const uint8_t function, const uint8_t expectedWords,
                                       const uint8_t replyWords, uint16_t *words,
                                       const size_t wordCapacity, uint8_t &receivedCount) {
    // An attempt waits for the controller's turnaround plus the time the
    // largest reply takes on the wire. What it costs the budget also counts
    // the request's own wire time.
    const uint32_t baud = _bus.baudRate();
    const uint8_t bitsPerCharacter = _bus.bitsPerCharacter();
    const uint32_t timeoutMs = WavinFrame::attemptTimeoutMs(WAVIN_TURNAROUND_MS, replyWords, baud,
                                                            bitsPerCharacter);
    const uint32_t attemptMs = WavinFrame::wireTimeMs(requestLength, baud, bitsPerCharacter) + timeoutMs;
    const uint32_t startedAt = millis();
    for (uint8_t attempt = 1U;; ++attempt) {
        const uint8_t status = transact(request, requestLength, slave, function, expectedWords, words,
                                        wordCapacity, receivedCount, timeoutMs);
        if (!isRetryable(status) || attempt >= attempts) return status;
        // The pause and a full attempt must still fit the transaction budget
        // the OTA quiescence is sized for; otherwise this failure is final.
        const uint32_t pauseMs = drawBackoffMs();
        if (!WavinFrame::attemptFits(millis() - startedAt, pauseMs, attemptMs,
                                     WAVIN_TRANSACTION_BUDGET_MS)) {
            return status;
        }
        delay(pauseMs);
    }
}

uint32_t WavinClient::drawBackoffMs() {
    const uint32_t span = WAVIN_RETRY_BACKOFF_MAX_MS - WAVIN_RETRY_BACKOFF_MIN_MS + 1U;
    return WAVIN_RETRY_BACKOFF_MIN_MS + esp_random() % span;
}

bool WavinClient::isRetryable(const uint8_t status) {
    return status == WavinFrame::kResponseTimedOut || status == WavinFrame::kInvalidCrc;
}

uint8_t WavinClient::transact(const uint8_t *request, const size_t requestLength, const uint8_t slave,
                              const uint8_t function, const uint8_t expectedWords, uint16_t *words,
                              const size_t wordCapacity, uint8_t &receivedCount,
                              const uint32_t timeoutMs) {
    Stream &io = _bus.stream();
    // Bytes left over from another master's traffic would be mistaken for
    // the start of our reply.
    while (io.available() > 0) (void)io.read();

    _bus.beginTransmit();
    io.write(request, requestLength);
    _bus.endTransmit();

    uint8_t buffer[WavinFrame::kMaxResponseSize];
    size_t received = 0U;
    bool crcMismatch = false;
    const uint32_t startedAt = millis();
    while (millis() - startedAt < timeoutMs) {
        if (io.available() <= 0) {
            delay(1);
            continue;
        }
        const int value = io.read();
        if (value < 0) continue;
        if (received == sizeof buffer) {
            std::memmove(buffer, buffer + 1, received - 1U);
            --received;
        }
        buffer[received++] = static_cast<uint8_t>(value);

        // The reply is looked for at every offset, so bytes ahead of it
        // (another master's traffic, or a candidate claiming a longer frame
        // than will arrive) cannot hide it; those that can no longer start
        // it are dropped.
        const WavinFrame::LocateResult found = WavinFrame::locateResponse(
            buffer, received, slave, function, expectedWords, words, wordCapacity);
        crcMismatch = crcMismatch || found.crcMismatch;
        if (found.status == WavinFrame::ScanStatus::Response) {
            receivedCount = found.wordCount;
            return WavinFrame::kSuccess;
        }
        if (found.status == WavinFrame::ScanStatus::Exception) return found.exceptionCode;
        if (found.dropCount > 0U) {
            std::memmove(buffer, buffer + found.dropCount, received - found.dropCount);
            received -= found.dropCount;
        }
    }
    // No more bytes will come: a candidate that failed its CRC but could
    // still have been the start of another master's request is now known to
    // be a corrupted frame.
    crcMismatch = crcMismatch
                  || WavinFrame::locateResponse(buffer, received, slave, function, expectedWords,
                                                words, wordCapacity, true).crcMismatch;
    return crcMismatch ? WavinFrame::kInvalidCrc : WavinFrame::kResponseTimedOut;
}
