#ifndef MODBUS_TO_MQTT_PROVISIONINGATTEMPTCORE_H
#define MODBUS_TO_MQTT_PROVISIONINGATTEMPTCORE_H

#include <cstdint>
#include <mutex>
#include <utility>

#include "Ipv4AddressCore.h"

namespace ProvisioningAttemptCore {

enum class State : std::uint8_t {
    Idle,
    Connecting,
    Connected,
    Failed,
    Disconnected,
};

enum class GotIpResult : std::uint8_t {
    Stale,
    Temporary,
    PersistenceRequired,
};

enum class PersistenceResult : std::uint8_t {
    Succeeded,
    CredentialFailed,
    NetworkConfigurationFailed,
};

template<typename Text>
struct Snapshot {
    State state = State::Idle;
    Text ssid;
    Text ip;
    Text reason;
    bool hasIp = false;
    bool provisioningReady = false;
};

template<typename Text, typename NetworkConfiguration>
struct Candidate {
    Text ssid;
    Text password;
    NetworkConfiguration networkConfiguration{};
    bool save = false;
};

template<typename Text, typename NetworkConfiguration>
class Controller {
public:
    using Token = std::uint32_t;
    using CandidateValue = Candidate<Text, NetworkConfiguration>;
    using SnapshotValue = Snapshot<Text>;

    Token start(CandidateValue candidate) {
        const std::lock_guard<std::mutex> guard(_mutex);
        ++_token;
        if (_token == 0U) ++_token;
        _candidate = std::move(candidate);
        _snapshot = {};
        _snapshot.state = State::Connecting;
        _snapshot.ssid = _candidate.ssid;
        _persistenceClaimed = false;
        return _token;
    }

    GotIpResult gotIp(const Token token, Text ip, CandidateValue &candidateOut) {
        const std::lock_guard<std::mutex> guard(_mutex);
        if (!isCurrentConnecting(token) || _persistenceClaimed) return GotIpResult::Stale;

        _snapshot.ip = std::move(ip);
        _snapshot.hasIp = Ipv4AddressCore::isValidStationAddress(_snapshot.ip);
        if (!_snapshot.hasIp) {
            _snapshot.ip = Text{};
            return GotIpResult::Stale;
        }

        if (!_candidate.save) {
            _snapshot.state = State::Connected;
            _snapshot.reason = Text("PERSISTENCE_REQUIRED");
            _snapshot.provisioningReady = false;
            clearSecrets();
            return GotIpResult::Temporary;
        }

        _persistenceClaimed = true;
        candidateOut = _candidate;
        return GotIpResult::PersistenceRequired;
    }

    bool completePersistence(const Token token, const PersistenceResult result) {
        const std::lock_guard<std::mutex> guard(_mutex);
        if (token == 0U || token != _token || !_persistenceClaimed
            || _snapshot.state != State::Connecting) {
            return false;
        }

        _persistenceClaimed = false;
        _snapshot.state = State::Connected;
        _snapshot.provisioningReady = result == PersistenceResult::Succeeded && _snapshot.hasIp;
        if (result == PersistenceResult::CredentialFailed) {
            _snapshot.reason = Text("CREDENTIAL_PERSIST_FAILED");
        } else if (result == PersistenceResult::NetworkConfigurationFailed) {
            _snapshot.reason = Text("NETWORK_CONFIG_PERSIST_FAILED");
        } else {
            _snapshot.reason = Text{};
        }
        clearSecrets();
        return true;
    }

    bool fail(const Token token, Text reason) {
        const std::lock_guard<std::mutex> guard(_mutex);
        if (token == 0U || token != _token || _snapshot.state != State::Connecting) return false;
        _snapshot.state = State::Failed;
        _snapshot.reason = std::move(reason);
        _snapshot.hasIp = false;
        _snapshot.ip = Text{};
        _snapshot.provisioningReady = false;
        _persistenceClaimed = false;
        clearSecrets();
        return true;
    }

    bool disconnect(const Token token, Text reason) {
        const std::lock_guard<std::mutex> guard(_mutex);
        if (token == 0U || token != _token) return false;
        if (_snapshot.state == State::Connecting) _snapshot.state = State::Failed;
        else if (_snapshot.state == State::Connected) _snapshot.state = State::Disconnected;
        else return false;
        _snapshot.reason = std::move(reason);
        _snapshot.hasIp = false;
        _snapshot.ip = Text{};
        _snapshot.provisioningReady = false;
        _persistenceClaimed = false;
        clearSecrets();
        return true;
    }

    bool cancel(const Token token) {
        return fail(token, Text("CANCELLED"));
    }

    bool isConnecting() const {
        const std::lock_guard<std::mutex> guard(_mutex);
        return _snapshot.state == State::Connecting;
    }

    bool isCurrent(const Token token) const {
        const std::lock_guard<std::mutex> guard(_mutex);
        return token != 0U && token == _token;
    }

    SnapshotValue snapshot() const {
        const std::lock_guard<std::mutex> guard(_mutex);
        return _snapshot;
    }

    void reset() {
        const std::lock_guard<std::mutex> guard(_mutex);
        ++_token;
        if (_token == 0U) ++_token;
        _snapshot = {};
        _candidate = {};
        _persistenceClaimed = false;
    }

private:
    bool isCurrentConnecting(const Token token) const {
        return token != 0U && token == _token && _snapshot.state == State::Connecting;
    }

    void clearSecrets() {
        _candidate.password = Text{};
    }

    mutable std::mutex _mutex;
    Token _token = 0U;
    SnapshotValue _snapshot{};
    CandidateValue _candidate{};
    bool _persistenceClaimed = false;
};

}  // namespace ProvisioningAttemptCore

#endif
