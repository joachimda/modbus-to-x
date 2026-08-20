#ifndef MODBUS_TO_MQTT_IPV4ADDRESSCORE_H
#define MODBUS_TO_MQTT_IPV4ADDRESSCORE_H

#include <cstddef>

namespace Ipv4AddressCore {

inline bool isValidStationAddress(const char *value, const std::size_t length) {
    if (value == nullptr || length < 7U || length > 15U) return false;

    std::size_t index = 0U;
    unsigned int addressValue = 0U;
    for (unsigned int octetIndex = 0U; octetIndex < 4U; ++octetIndex) {
        const std::size_t octetStart = index;
        unsigned int octet = 0U;
        while (index < length && value[index] != '.') {
            if (value[index] < '0' || value[index] > '9' || index - octetStart >= 3U) return false;
            octet = octet * 10U + static_cast<unsigned int>(value[index] - '0');
            if (octet > 255U) return false;
            ++index;
        }
        if (index == octetStart || (index - octetStart > 1U && value[octetStart] == '0')) return false;
        addressValue |= octet;

        if (octetIndex < 3U) {
            if (index >= length || value[index] != '.') return false;
            ++index;
        } else if (index != length) {
            return false;
        }
    }
    return addressValue != 0U;
}

template<typename Text>
bool isValidStationAddress(const Text &value) {
    return isValidStationAddress(value.c_str(), value.length());
}

}  // namespace Ipv4AddressCore

#endif
