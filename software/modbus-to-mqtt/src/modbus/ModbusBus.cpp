#include "modbus/ModbusBus.h"

#include <map>

#include "Config.h"
#include "Logger.h"

using SerialModeMap = std::map<String, uint32_t>;
static const SerialModeMap kSerialModes = {
    {"8N1", SERIAL_8N1},
    {"8N2", SERIAL_8N2},
    {"8E1", SERIAL_8E1},
    {"8E2", SERIAL_8E2},
    {"8O1", SERIAL_8O1},
    {"8O2", SERIAL_8O2}
};

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

ModbusBus::ModbusBus(Logger *logger) : _logger(logger) {
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

uint32_t ModbusBus::getErrorCount() {
    if (s_instance == nullptr) {
        return 0;
    }
    return s_instance->errorCount();
}

bool ModbusBus::isEnabled() {
    return s_instance != nullptr && s_instance->isActive();
}

void ModbusBus::setEnabled(const bool enabled) {
    if (s_instance != nullptr) {
        s_instance->setActive(enabled);
    }
}

bool ModbusBus::isBusy() const {
    return _busy.load(std::memory_order_acquire);
}

void ModbusBus::enableCapture(const bool enable) {
    if (_tee) {
        _tee->enableCapture(enable);
    }
}

void ModbusBus::initializeWiring(const Bus &busConfig) {
    pinMode(RS485_DERE_PIN, OUTPUT);
    digitalWrite(RS485_DERE_PIN, LOW);

    const auto formatIt = kSerialModes.find(busConfig.serialFormat);
    const uint32_t mode = (formatIt != kSerialModes.end()) ? formatIt->second : SERIAL_8N1;
    const uint32_t baud = busConfig.baud ? busConfig.baud : DEFAULT_MODBUS_BAUD_RATE;

    Serial1.begin(baud, mode, RX2, TX2);

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
