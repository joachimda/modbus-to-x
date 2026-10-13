#ifndef MODBUS_TO_MQTT_MODBUS_OWNER_CORE_H
#define MODBUS_TO_MQTT_MODBUS_OWNER_CORE_H

#include <cstddef>
#include <cstdint>
#include <utility>

// Platform-neutral transaction-boundary model. The firmware adapter observes
// priority control work before a transaction and never starts a second one
// after OTA admission closes.
template<typename Configuration, typename Bus>
class ModbusOwnerCore {
public:
    struct Snapshot {
        uint32_t generation{0U};
        size_t deviceCount{0U};
        size_t datapointCount{0U};
        bool quiesced{false};
        bool active{false};
        uint32_t otaOperationId{0U};
    };

    explicit ModbusOwnerCore(Bus &bus)
        : _bus(bus) {}

    void requestActivation(Configuration candidate) {
        if (!_ordinaryAdmissionOpen) return;
        _pending = std::move(candidate);
        _hasPending = true;
    }

    bool processBoundary() {
        if (observeQuiesceBoundary()) return false;
        if (_polling || _commandActive || !_hasPending || _quiesced) return false;
        _bus.initialize(_pending.busValue);
        _active = std::move(_pending);
        _hasPending = false;
        ++_generation;
        return true;
    }

    template<typename Visitor>
    void runPollingPass(Visitor &&visitor) {
        if (_quiesced || !_ordinaryAdmissionOpen || !_activeBus) return;
        for (size_t i = 0; i < _active.deviceCount; ++i) {
            if (observeQuiesceBoundary()) break;
            _polling = true;
            visitor(_active, i);
            _polling = false;
            (void)observeQuiesceBoundary();
            break;
        }
    }

    template<typename Operation>
    void runCommand(Operation &&operation) {
        if (_quiesced || !_ordinaryAdmissionOpen) return;
        _commandActive = true;
        operation(_active);
        _commandActive = false;
        (void)observeQuiesceBoundary();
    }

    const Configuration &active() const { return _active; }

    void setActive(const bool active) { _activeBus = active; }

    bool quiesceForOta(const uint32_t operationId) {
        if (!requestQuiesceForOta(operationId)) return false;
        (void)observeQuiesceBoundary();
        return _quiesced && _otaOperationId == operationId;
    }

    bool requestQuiesceForOta(const uint32_t operationId) {
        if (operationId == 0U) return false;
        if (_quiesced) return _otaOperationId == operationId;
        if (_quiescePending) return _pendingOtaOperationId == operationId;
        _ordinaryAdmissionOpen = false;
        _quiescePending = true;
        _pendingOtaOperationId = operationId;
        return true;
    }

    bool resumeFromOta(const uint32_t operationId) {
        if (!_quiesced || operationId != _otaOperationId) return false;
        _activeBus = _wasActiveBeforeOta;
        _quiesced = false;
        _otaOperationId = 0U;
        _ordinaryAdmissionOpen = true;
        return true;
    }

    Snapshot snapshot() const {
        Snapshot value;
        value.generation = _generation;
        value.deviceCount = _active.deviceCount;
        value.datapointCount = _active.datapointCount;
        value.quiesced = _quiesced;
        value.active = _activeBus;
        value.otaOperationId = _otaOperationId;
        return value;
    }

private:
    bool observeQuiesceBoundary() {
        if (!_quiescePending || _polling || _commandActive) return false;
        _wasActiveBeforeOta = _activeBus;
        _activeBus = false;
        _quiesced = true;
        _otaOperationId = _pendingOtaOperationId;
        _pendingOtaOperationId = 0U;
        _quiescePending = false;
        return true;
    }

    Bus &_bus;
    Configuration _active{};
    Configuration _pending{};
    bool _hasPending{false};
    bool _polling{false};
    bool _commandActive{false};
    bool _activeBus{false};
    bool _quiesced{false};
    bool _quiescePending{false};
    bool _ordinaryAdmissionOpen{true};
    bool _wasActiveBeforeOta{false};
    uint32_t _otaOperationId{0U};
    uint32_t _pendingOtaOperationId{0U};
    uint32_t _generation{0U};
};

#endif
