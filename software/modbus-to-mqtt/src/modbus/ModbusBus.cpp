#include "modbus/ModbusBus.h"

ModbusBus *ModbusBus::s_instance = nullptr;

ModbusBus::ModbusBus(Logger *logger) : _port(logger) {
    s_instance = this;
    _node.preTransmission(preTransmitTrampoline);
    _node.postTransmission(postTransmitTrampoline);
}

bool ModbusBus::begin(const Bus &busConfig) {
    return _port.begin(busConfig);
}

Rs485Port &ModbusBus::port() {
    return _port;
}

ModbusMaster &ModbusBus::node() {
    return _node;
}

bool ModbusBus::isActive() const {
    return _active.load(std::memory_order_acquire);
}

void ModbusBus::setActive(const bool enabled) {
    _active.store(enabled, std::memory_order_release);
}

void ModbusBus::preTransmitTrampoline() {
    if (s_instance) {
        s_instance->_port.beginTransmit();
    }
}

void ModbusBus::postTransmitTrampoline() {
    if (s_instance) {
        s_instance->_port.endTransmit();
    }
}
