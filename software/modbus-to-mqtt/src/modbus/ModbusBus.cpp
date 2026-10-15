#include "modbus/ModbusBus.h"

#include <map>

#include "Config.h"
#include "Logger.h"

// A serial format name's UART configuration and the bits one character
// takes on the wire (start, data, parity and stop bits). No entry may exceed
// WavinFrame::kMaxBitsPerCharacter, which the Wavin timing is checked
// against at compile time.
struct SerialFormatSpec {
    uint32_t config;
    uint8_t bitsPerCharacter;
};

using SerialFormatMap = std::map<String, SerialFormatSpec>;
static const SerialFormatMap kSerialFormats = {
    {"8N1", {SERIAL_8N1, 10U}},
    {"8N2", {SERIAL_8N2, 11U}},
    {"8E1", {SERIAL_8E1, 11U}},
    {"8E2", {SERIAL_8E2, 12U}},
    {"8O1", {SERIAL_8O1, 11U}},
    {"8O2", {SERIAL_8O2, 12U}}
};
static const SerialFormatSpec kDefaultSerialFormat = {SERIAL_8N1, 10U};

ModbusBus *ModbusBus::s_instance = nullptr;

ModbusBus::Guard::Guard(ModbusBus &bus, const bool owns)
    : _bus(&bus), _owns(owns) {
}

ModbusBus::Guard::Guard(ModbusBus::Guard &&other) noexcept
    : _bus(other._bus), _owns(other._owns) {
    other._bus = nullptr;
    other._owns = false;
}

auto ModbusBus::Guard::operator=(ModbusBus::Guard &&other) noexcept -> ModbusBus::Guard & {
    if (this != &other) {
        if (_owns && _bus) {
            _bus->release();
        }
        _bus = other._bus;
        _owns = other._owns;
        other._bus = nullptr;
        other._owns = false;
    }
    return *this;
}

ModbusBus::Guard::~Guard() {
    if (_owns && _bus) {
        _bus->release();
    }
}

ModbusBus::ModbusBus(Logger *logger)
    : _logger(logger),
      _baud(DEFAULT_MODBUS_BAUD_RATE),
      _bitsPerCharacter(kDefaultSerialFormat.bitsPerCharacter) {
    s_instance = this;
    _node.preTransmission(preTransmitTrampoline);
    _node.postTransmission(postTransmitTrampoline);
}

bool ModbusBus::begin(const Bus &busConfig) {
    initializeWiring(busConfig);
    _initialized.store(true, std::memory_order_release);
    return true;
}

auto ModbusBus::acquire() -> Guard {
    bool expected = false;
    if (!_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return Guard(*this, false);
    }
    return Guard(*this, true);
}

ModbusMaster &ModbusBus::node() {
    return _node;
}

Stream &ModbusBus::stream() {
    if (_tee) {
        return *_tee;
    }
    return Serial1;
}

uint32_t ModbusBus::baudRate() const {
    return _baud;
}

uint8_t ModbusBus::bitsPerCharacter() const {
    return _bitsPerCharacter;
}

bool ModbusBus::isActive() const {
    return _active.load(std::memory_order_acquire);
}

void ModbusBus::setActive(const bool enabled) {
    _active.store(enabled, std::memory_order_release);
}

bool ModbusBus::isInitialized() const {
    return _initialized.load(std::memory_order_acquire);
}

void ModbusBus::incrementError() {
    _errorCount.fetch_add(1, std::memory_order_relaxed);
}

uint32_t ModbusBus::errorCount() const {
    return _errorCount.load(std::memory_order_relaxed);
}

String ModbusBus::dumpRx() const {
    if (_tee) {
        return _tee->dumpHex();
    }
    return String();
}

void ModbusBus::enableCapture(const bool enable) {
    if (_tee) {
        _tee->enableCapture(enable);
    }
}

void ModbusBus::beginTransmit() {
    onPreTransmission();
}

void ModbusBus::endTransmit() {
    onPostTransmission();
}

void ModbusBus::initializeWiring(const Bus &busConfig) {
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
}

void ModbusBus::onPreTransmission() {
    digitalWrite(RS485_DERE_PIN, HIGH);
    if (_tee) _tee->enableCapture(false);
    delayMicroseconds(RS485_DIR_GUARD_US);
}

void ModbusBus::onPostTransmission() {
    Serial1.flush();
    digitalWrite(RS485_DERE_PIN, LOW);
    if (_tee) _tee->enableCapture(true);
    delayMicroseconds(RS485_DIR_GUARD_US);
}

void ModbusBus::preTransmitTrampoline() {
    if (s_instance) {
        s_instance->onPreTransmission();
    }
}

void ModbusBus::postTransmitTrampoline() {
    if (s_instance) {
        s_instance->onPostTransmission();
    }
}

void ModbusBus::release() {
    _busy.store(false, std::memory_order_release);
}
