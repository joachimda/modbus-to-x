#ifndef MODBUS_TO_MQTT_OWNER_COMPLETION_CORE_H
#define MODBUS_TO_MQTT_OWNER_COMPLETION_CORE_H

#include <atomic>
#include <cstdint>

class OwnerCompletionCore {
public:
    enum class State : uint8_t {
        Created,
        Queued,
        Running,
        Completed,
        Cancelled,
    };

    bool markQueued() { return transition(State::Created, State::Queued); }
    bool tryStart() { return transition(State::Queued, State::Running); }
    bool cancelIfQueued() { return transition(State::Queued, State::Cancelled); }
    bool complete() { return transition(State::Running, State::Completed); }

    State state() const { return _state.load(std::memory_order_acquire); }

private:
    bool transition(const State from, const State to) {
        State expected = from;
        return _state.compare_exchange_strong(expected, to, std::memory_order_acq_rel);
    }

    std::atomic<State> _state{State::Created};
};

#endif
