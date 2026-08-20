#ifndef MODBUS_TO_MQTT_PROVISIONINGEVENTGATE_H
#define MODBUS_TO_MQTT_PROVISIONINGEVENTGATE_H

#include <cstdint>

namespace ProvisioningEventGate {

enum class Event : std::uint8_t {
    Other,
    StationStopped,
    StationConnected,
    StationGotIp,
    StationDisconnected,
};

enum class Action : std::uint8_t {
    Ignore,
    StartConnection,
    Deliver,
};

template<typename Token>
struct Decision {
    Decision() = default;
    Decision(const Action actionValue, const Token tokenValue) : action(actionValue), token(tokenValue) {}

    Action action = Action::Ignore;
    Token token{};
};

template<typename Token>
class Gate {
public:
    void awaitStationStop(const Token token) {
        _token = token;
        _state = token == Token{} ? State::Idle : State::AwaitingStationStop;
    }

    Decision<Token> onEvent(const Event event) {
        if (_state == State::AwaitingStationStop) {
            if (event != Event::StationStopped) return {};
            _state = State::Active;
            return {Action::StartConnection, _token};
        }
        if (_state == State::Active) return {Action::Deliver, _token};
        return {};
    }

    void cancel(const Token token) {
        if (token != Token{} && token == _token) reset();
    }

    void reset() {
        _state = State::Idle;
        _token = Token{};
    }

private:
    enum class State : std::uint8_t {
        Idle,
        AwaitingStationStop,
        Active,
    };

    State _state = State::Idle;
    Token _token{};
};

}  // namespace ProvisioningEventGate

#endif
