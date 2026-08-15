#ifndef MODBUS_TO_MQTT_MODBUS_OWNER_CORE_H
#define MODBUS_TO_MQTT_MODBUS_OWNER_CORE_H

#include <cstddef>
#include <cstdint>
#include <utility>

// Platform-neutral activation-boundary model. The firmware adapter follows the
// same rule by draining its queue only before a complete polling pass.
template<typename Configuration, typename Bus>
class ModbusOwnerCore {
public:
    struct Snapshot {
        uint32_t generation{0U};
        size_t deviceCount{0U};
        size_t datapointCount{0U};
    };

    explicit ModbusOwnerCore(Bus &bus)
        : _bus(bus) {}

    void requestActivation(Configuration candidate) {
        _pending = std::move(candidate);
        _hasPending = true;
    }

    bool processBoundary() {
        if (_polling || _commandActive || !_hasPending) return false;
        _bus.initialize(_pending.busValue);
        _active = std::move(_pending);
        _hasPending = false;
        ++_generation;
        return true;
    }

    template<typename Visitor>
    void runPollingPass(Visitor &&visitor) {
        _polling = true;
        for (size_t i = 0; i < _active.deviceCount; ++i) {
            visitor(_active, i);
        }
        _polling = false;
    }

    template<typename Operation>
    void runCommand(Operation &&operation) {
        _commandActive = true;
        operation(_active);
        _commandActive = false;
    }

    const Configuration &active() const { return _active; }

    Snapshot snapshot() const {
        Snapshot value;
        value.generation = _generation;
        value.deviceCount = _active.deviceCount;
        value.datapointCount = _active.datapointCount;
        return value;
    }

private:
    Bus &_bus;
    Configuration _active{};
    Configuration _pending{};
    bool _hasPending{false};
    bool _polling{false};
    bool _commandActive{false};
    uint32_t _generation{0U};
};

#endif
