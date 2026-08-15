#include "mqtt/MqttConfigCore.h"

#include <cstring>

namespace MqttConfigCore {
namespace {

bool fail(ValidationError &error, const Field field, const Constraint constraint) {
    error = ValidationError(field, constraint);
    return false;
}

bool validateMaximum(const std::string &value, const size_t maximum,
                     const Field field, ValidationError &error) {
    if (value.size() > maximum) return fail(error, field, Constraint::MaximumBytes);
    return true;
}

bool parsePort(const std::string &text, uint16_t &port, ValidationError &error) {
    if (text.empty() || text.size() > PORT_MAX_BYTES) {
        return fail(error, Field::BrokerPort,
                    text.size() > PORT_MAX_BYTES ? Constraint::MaximumBytes : Constraint::PortDecimal);
    }

    uint32_t value = 0U;
    for (const unsigned char ch : text) {
        if (ch < '0' || ch > '9') return fail(error, Field::BrokerPort, Constraint::PortDecimal);
        value = value * 10U + static_cast<uint32_t>(ch - '0');
    }
    if (value < PORT_MIN || value > PORT_MAX) {
        return fail(error, Field::BrokerPort, Constraint::PortRange);
    }
    port = static_cast<uint16_t>(value);
    return true;
}

bool prepareNonSecret(const ConnectionInput &input, std::string &broker,
                      std::string &user, uint16_t &port, ValidationError &error) {
    const std::string brokerIp = trim(input.brokerIp);
    const std::string brokerUrl = trim(input.brokerUrl);
    const std::string brokerUrlHost = extractHost(brokerUrl);
    const std::string brokerPort = trim(input.brokerPort);
    user = trim(input.user);

    if (!validateMaximum(brokerIp, BROKER_MAX_BYTES, Field::BrokerIp, error)
        || !validateMaximum(brokerUrlHost, BROKER_MAX_BYTES, Field::BrokerUrlHost, error)
        || !validateMaximum(user, USER_MAX_BYTES, Field::User, error)) {
        return false;
    }

    if (!brokerIp.empty() && brokerIp != DEFAULT_BROKER) broker = brokerIp;
    else if (!brokerUrl.empty()) broker = brokerUrlHost;
    else broker = DEFAULT_BROKER;

    if (!validateMaximum(broker, BROKER_MAX_BYTES, Field::Broker, error)) return false;
    return parsePort(brokerPort, port, error);
}

}  // namespace

std::string trim(const std::string &value) {
    size_t begin = 0U;
    while (begin < value.size() && static_cast<unsigned char>(value[begin]) <= ' ') ++begin;

    size_t end = value.size();
    while (end > begin && static_cast<unsigned char>(value[end - 1U]) <= ' ') --end;
    return value.substr(begin, end - begin);
}

std::string extractHost(const std::string &url) {
    if (url.empty()) return {};

    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string::npos ? 0U : scheme + 3U;
    const size_t slash = url.find('/', start);
    const size_t colon = url.find(':', start);
    size_t end = url.size();
    if (slash != std::string::npos && slash < end) end = slash;
    if (colon != std::string::npos && colon < end) end = colon;
    return url.substr(start, end - start);
}

bool validateNonSecret(const ConnectionInput &input, ValidationError &error) {
    std::string broker;
    std::string user;
    uint16_t port = 0U;
    error = ValidationError();
    return prepareNonSecret(input, broker, user, port, error);
}

bool validatePassword(const std::string &password, ValidationError &error) {
    error = ValidationError();
    return validateMaximum(password, PASSWORD_MAX_BYTES, Field::Password, error);
}

bool prepareConnection(const ConnectionInput &input, PreparedConnection &output, ValidationError &error) {
    output = PreparedConnection();
    error = ValidationError();

    std::string broker;
    std::string user;
    uint16_t port = 0U;
    if (!prepareNonSecret(input, broker, user, port, error)
        || !validatePassword(input.password, error)) {
        return false;
    }

    PreparedConnection candidate;
    if (!copyCString(candidate.broker.data(), candidate.broker.size(), broker)) {
        return fail(error, Field::Broker, Constraint::MaximumBytes);
    }
    if (!copyCString(candidate.user.data(), candidate.user.size(), user)) {
        return fail(error, Field::User, Constraint::MaximumBytes);
    }
    if (!copyCString(candidate.password.data(), candidate.password.size(), input.password)) {
        return fail(error, Field::Password, Constraint::MaximumBytes);
    }
    candidate.port = port;
    candidate.valid = true;
    output = candidate;
    return true;
}

bool copyCString(char *destination, const size_t capacity, const std::string &value) {
    if (destination == nullptr || capacity == 0U || value.size() >= capacity) return false;
    if (!value.empty()) std::memcpy(destination, value.data(), value.size());
    destination[value.size()] = '\0';
    return true;
}

const char *fieldName(const Field field) {
    switch (field) {
        case Field::BrokerIp: return "broker_ip";
        case Field::BrokerUrlHost: return "broker_url";
        case Field::Broker: return "broker";
        case Field::BrokerPort: return "broker_port";
        case Field::User: return "user";
        case Field::Password: return "password";
        case Field::None: break;
    }
    return "unknown";
}

const char *constraintMessage(const ValidationError &error) {
    if (error.constraint == Constraint::MaximumBytes) {
        switch (error.field) {
            case Field::BrokerIp:
            case Field::BrokerUrlHost:
            case Field::Broker:
            case Field::User:
            case Field::Password: return "exceeds the maximum UTF-8 byte length";
            case Field::BrokerPort: return "exceeds the maximum ASCII decimal byte length";
            case Field::None: break;
        }
    }
    if (error.constraint == Constraint::PortDecimal) return "must contain one or more ASCII decimal digits";
    if (error.constraint == Constraint::PortRange) return "is outside the supported port range";
    return "is invalid";
}

size_t maximumBytes(const Field field) {
    switch (field) {
        case Field::BrokerIp:
        case Field::BrokerUrlHost:
        case Field::Broker: return BROKER_MAX_BYTES;
        case Field::BrokerPort: return PORT_MAX_BYTES;
        case Field::User: return USER_MAX_BYTES;
        case Field::Password: return PASSWORD_MAX_BYTES;
        case Field::None: break;
    }
    return 0U;
}

}  // namespace MqttConfigCore
