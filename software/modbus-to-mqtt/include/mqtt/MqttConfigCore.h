#ifndef MODBUS_TO_MQTT_MQTTCONFIGCORE_H
#define MODBUS_TO_MQTT_MQTTCONFIGCORE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace MqttConfigCore {

// Authoritative MQTT connection-field limits. Sizes exclude the trailing NUL
// retained by PreparedConnection and are measured in UTF-8/storage bytes.
constexpr size_t BROKER_MAX_BYTES = 149U;
constexpr size_t PORT_MAX_BYTES = 5U;
constexpr size_t USER_MAX_BYTES = 31U;
constexpr size_t PASSWORD_MAX_BYTES = 31U;
constexpr uint16_t PORT_MIN = 1U;
constexpr uint16_t PORT_MAX = 65535U;

constexpr const char *DEFAULT_BROKER = "0.0.0.0";
constexpr const char *DEFAULT_PORT = "1883";
constexpr const char *DEFAULT_ROOT_TOPIC = "mbx_root";

enum class Field {
    None,
    BrokerIp,
    BrokerUrlHost,
    Broker,
    BrokerPort,
    User,
    Password,
};

enum class Constraint {
    None,
    MaximumBytes,
    PortDecimal,
    PortRange,
};

struct ValidationError {
    ValidationError(const Field fieldValue = Field::None,
                    const Constraint constraintValue = Constraint::None)
        : field(fieldValue), constraint(constraintValue) {}

    Field field;
    Constraint constraint;
};

struct ConnectionInput {
    std::string brokerIp;
    std::string brokerUrl;
    std::string brokerPort;
    std::string user;
    std::string password;
};

struct PreparedConnection {
    std::array<char, BROKER_MAX_BYTES + 1U> broker{};
    std::array<char, USER_MAX_BYTES + 1U> user{};
    std::array<char, PASSWORD_MAX_BYTES + 1U> password{};
    uint16_t port = 0U;
    bool valid = false;
};

std::string trim(const std::string &value);

std::string extractHost(const std::string &url);

bool validateNonSecret(const ConnectionInput &input, ValidationError &error);

bool validatePassword(const std::string &password, ValidationError &error);

bool prepareConnection(const ConnectionInput &input, PreparedConnection &output, ValidationError &error);

bool copyCString(char *destination, size_t capacity, const std::string &value);

const char *fieldName(Field field);

const char *constraintMessage(const ValidationError &error);

size_t maximumBytes(Field field);

template<typename Callback>
bool withValidConnection(const PreparedConnection &connection, Callback &&callback) {
    if (!connection.valid) return false;
    return std::forward<Callback>(callback)(connection);
}

// A failed load is responsible for leaving MQTT disabled. After a valid load,
// always restore the persisted enabled preference, regardless of whether the
// one-shot connection attempt succeeds.
template<typename LoadCallback, typename AttemptCallback, typename RestoreCallback>
bool runOneShotConnectionTest(LoadCallback &&load, AttemptCallback &&attempt,
                              RestoreCallback &&restore) {
    bool configuredEnabled = false;
    if (!std::forward<LoadCallback>(load)(configuredEnabled)) return false;

    const bool connected = std::forward<AttemptCallback>(attempt)();
    std::forward<RestoreCallback>(restore)(configuredEnabled);
    return connected;
}

}  // namespace MqttConfigCore

#endif
