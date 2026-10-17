#include "modbus/Rs485Port.h"

#include <map>

#include "Config.h"
#include "Logger.h"
#include "utils/TeeStream.h"

namespace {

// A serial format name's UART configuration and the bits one character
// takes on the wire (start, data, parity and stop bits). No entry may exceed
// WavinFrame::kMaxBitsPerCharacter, which the Wavin timing is checked
// against at compile time.
struct SerialFormatSpec {
    uint32_t config;
    uint8_t bitsPerCharacter;
};

using SerialFormatMap = std::map<String, SerialFormatSpec>;
const SerialFormatMap kSerialFormats = {
    {"8N1", {SERIAL_8N1, 10U}},
    {"8N2", {SERIAL_8N2, 11U}},
    {"8E1", {SERIAL_8E1, 11U}},
    {"8E2", {SERIAL_8E2, 12U}},
    {"8O1", {SERIAL_8O1, 11U}},
    {"8O2", {SERIAL_8O2, 12U}}
};
const SerialFormatSpec kDefaultSerialFormat = {SERIAL_8N1, 10U};

}  // namespace

Rs485Port::Guard::Guard(Rs485Port &port, const bool owns)
    : _port(&port), _owns(owns) {
}

Rs485Port::Guard::Guard(Rs485Port::Guard &&other) noexcept
    : _port(other._port), _owns(other._owns) {
    other._port = nullptr;
    other._owns = false;
}

auto Rs485Port::Guard::operator=(Rs485Port::Guard &&other) noexcept -> Rs485Port::Guard & {
    if (this != &other) {
        if (_owns && _port) {
            _port->release();
        }
        _port = other._port;
        _owns = other._owns;
        other._port = nullptr;
        other._owns = false;
    }
    return *this;
}

Rs485Port::Guard::~Guard() {
    if (_owns && _port) {
        _port->release();
    }
}

Rs485Port::Rs485Port(Logger *logger)
    : _logger(logger),
      _baud(DEFAULT_MODBUS_BAUD_RATE),
      _bitsPerCharacter(kDefaultSerialFormat.bitsPerCharacter) {
}

bool Rs485Port::begin(const Bus &busConfig) {
    pinMode(RS485_DERE_PIN, OUTPUT);
    digitalWrite(RS485_DERE_PIN, LOW);

    const auto formatIt = kSerialFormats.find(busConfig.serialFormat);
    const SerialFormatSpec &format =
        (formatIt != kSerialFormats.end()) ? formatIt->second : kDefaultSerialFormat;
    const uint32_t baud = busConfig.baud ? busConfig.baud : DEFAULT_MODBUS_BAUD_RATE;
    _baud = baud;
    _bitsPerCharacter = format.bitsPerCharacter;

    Serial1.begin(baud, format.config, RX2, TX2);

    if (!_tee) {
        _tee = new TeeStream(Serial1, _logger);
    }
    _tee->enableCapture(true);
    _initialized.store(true, std::memory_order_release);
    return true;
}

bool Rs485Port::isInitialized() const {
    return _initialized.load(std::memory_order_acquire);
}

auto Rs485Port::acquire() -> Guard {
    bool expected = false;
    if (!_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return Guard(*this, false);
    }
    select();
    return Guard(*this, true);
}

void Rs485Port::select() {
}

Stream &Rs485Port::stream() {
    if (_tee) {
        return *_tee;
    }
    return Serial1;
}

uint32_t Rs485Port::baudRate() const {
    return _baud;
}

uint8_t Rs485Port::bitsPerCharacter() const {
    return _bitsPerCharacter;
}

void Rs485Port::send(const uint8_t *data, const size_t length) {
    beginTransmit();
    stream().write(data, length);
    endTransmit();
}

void Rs485Port::beginTransmit() {
    digitalWrite(RS485_DERE_PIN, HIGH);
    if (_tee) _tee->enableCapture(false);
    delayMicroseconds(RS485_DIR_GUARD_US);
}

void Rs485Port::endTransmit() {
    Serial1.flush();
    digitalWrite(RS485_DERE_PIN, LOW);
    if (_tee) _tee->enableCapture(true);
    delayMicroseconds(RS485_DIR_GUARD_US);
}

void Rs485Port::discardInput() {
    Stream &io = stream();
    while (io.available() > 0) (void)io.read();
}

void Rs485Port::waitForQuietLine(const uint32_t lastByteAtMs, const uint32_t quietMs,
                                 const uint32_t maxMs) {
    Stream &io = stream();
    const uint32_t startedAt = millis();
    uint32_t quietSince = lastByteAtMs;
    // Measured from the caller's last byte: after a wait in which nothing
    // arrived the line is already quiet and this returns at once. The cap
    // keeps a talker that never stops from holding the caller.
    while (millis() - quietSince < quietMs && millis() - startedAt < maxMs) {
        if (io.available() > 0) {
            (void)io.read();
            quietSince = millis();
            continue;
        }
        delay(1);
    }
}

void Rs485Port::incrementError() {
    _errorCount.fetch_add(1, std::memory_order_relaxed);
}

uint32_t Rs485Port::errorCount() const {
    return _errorCount.load(std::memory_order_relaxed);
}

String Rs485Port::dumpRx() const {
    if (_tee) {
        return _tee->dumpHex();
    }
    return String();
}

void Rs485Port::enableCapture(const bool enable) {
    if (_tee) {
        _tee->enableCapture(enable);
    }
}

void Rs485Port::release() {
    _busy.store(false, std::memory_order_release);
}
