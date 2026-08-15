#ifndef MODBUS_TO_MQTT_MQTT_OWNER_CORE_H
#define MODBUS_TO_MQTT_MQTT_OWNER_CORE_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Platform-neutral behavioral core used by native tests to pin the ordering,
// generation and dispatch rules implemented by the FreeRTOS/PubSubClient
// adapter in MqttManager.
template<typename Client>
class MqttOwnerCore {
public:
    using Handler = std::function<void(const std::string &)>;

    struct Subscription {
        std::string topic;
        Handler handler;
    };

    struct Snapshot {
        std::string server;
        std::vector<std::string> topics;
        bool enabled{false};
        uint32_t generation{0U};
        uint32_t modbusGeneration{0U};
    };

    MqttOwnerCore(Client &client, const uintptr_t ownerContext)
        : _client(client), _ownerContext(ownerContext) {}

    bool applyConfiguration(const uintptr_t context, const std::string &server, const bool enabled) {
        if (!checkOwner(context)) return false;
        if (_connected) {
            _client.disconnect();
            _connected = false;
        }
        _client.setServer(server);
        _server = server;
        _enabled = enabled;
        ++_generation;
        return true;
    }

    bool replaceSubscriptions(const uintptr_t context, std::vector<Subscription> subscriptions,
                              const uint32_t modbusGeneration) {
        if (!checkOwner(context)) return false;
        std::vector<std::string> seen;
        for (const auto &subscription : subscriptions) {
            if (subscription.topic.empty()
                || std::find(seen.begin(), seen.end(), subscription.topic) != seen.end()) return false;
            seen.push_back(subscription.topic);
        }
        if (_connected) {
            for (const auto &oldSubscription : _subscriptions) {
                if (std::find(seen.begin(), seen.end(), oldSubscription.topic) == seen.end()) {
                    _client.unsubscribe(oldSubscription.topic);
                }
            }
            for (const auto &subscription : subscriptions) {
                const auto found = std::find_if(_subscriptions.begin(), _subscriptions.end(),
                    [&](const Subscription &oldSubscription) {
                        return oldSubscription.topic == subscription.topic;
                    });
                if (found == _subscriptions.end()) _client.subscribe(subscription.topic);
            }
        }
        _subscriptions = std::move(subscriptions);
        _modbusGeneration = modbusGeneration;
        ++_generation;
        return true;
    }

    bool connect(const uintptr_t context) {
        if (!checkOwner(context) || !_enabled) return false;
        _connected = _client.connect();
        if (_connected) {
            for (const auto &subscription : _subscriptions) _client.subscribe(subscription.topic);
        }
        return _connected;
    }

    bool loop(const uintptr_t context) {
        if (!checkOwner(context)) return false;
        return _client.loop();
    }

    bool publish(const uintptr_t context, const std::string &topic, const std::string &payload,
                 const uint32_t mqttGeneration, const uint32_t modbusGeneration) {
        if (!checkOwner(context) || !_connected || mqttGeneration != _generation
            || modbusGeneration != _modbusGeneration) return false;
        return _client.publish(topic, payload);
    }

    bool disable(const uintptr_t context) {
        if (!checkOwner(context)) return false;
        _enabled = false;
        if (_connected) _client.disconnect();
        _connected = false;
        return true;
    }

    bool dispatch(const uintptr_t context, const std::string &topic, const std::string &payload) {
        if (!checkOwner(context)) return false;
        Handler selected;
        for (const auto &subscription : _subscriptions) {
            if (subscription.topic == topic) {
                selected = subscription.handler;
                break;
            }
        }
        if (!selected) return false;
        selected(payload);
        return true;
    }

    uint32_t generation() const { return _generation; }
    uint32_t ownerViolations() const { return _ownerViolations; }

    Snapshot snapshot() const {
        Snapshot value;
        value.server = _server;
        value.enabled = _enabled;
        value.generation = _generation;
        value.modbusGeneration = _modbusGeneration;
        value.topics.reserve(_subscriptions.size());
        for (const auto &subscription : _subscriptions) value.topics.push_back(subscription.topic);
        return value;
    }

private:
    bool checkOwner(const uintptr_t context) {
        if (context == _ownerContext) return true;
        ++_ownerViolations;
        return false;
    }

    Client &_client;
    uintptr_t _ownerContext;
    bool _enabled{false};
    bool _connected{false};
    std::string _server;
    uint32_t _generation{0U};
    uint32_t _modbusGeneration{0U};
    uint32_t _ownerViolations{0U};
    std::vector<Subscription> _subscriptions;
};

#endif
