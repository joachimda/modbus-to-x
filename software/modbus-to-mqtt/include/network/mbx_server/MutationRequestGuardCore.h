#ifndef MODBUS_TO_MQTT_MUTATIONREQUESTGUARDCORE_H
#define MODBUS_TO_MQTT_MUTATIONREQUESTGUARDCORE_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

namespace MutationRequestGuardCore {

constexpr const char *REQUEST_HEADER = "X-MBX-Request";
constexpr const char *REQUEST_HEADER_VALUE = "1";
constexpr const char *FORBIDDEN_RESPONSE = R"({"error":"forbidden_request_context"})";

inline bool hasValidMarker(const char *value) {
    return value != nullptr && std::string(value) == REQUEST_HEADER_VALUE;
}

inline bool normalizeHost(const char *rawHost, std::string &normalized) {
    if (rawHost == nullptr || *rawHost == '\0') return false;

    std::string host(rawHost);
    if (host.find_first_of(" ,/@[]") != std::string::npos) return false;

    const size_t colon = host.find(':');
    if (colon != std::string::npos) {
        if (colon == 0U || colon != host.rfind(':') || colon + 1U == host.size()) return false;
        const std::string port = host.substr(colon + 1U);
        if (!std::all_of(port.begin(), port.end(), [](const unsigned char ch) { return std::isdigit(ch) != 0; })) {
            return false;
        }
        char *end = nullptr;
        const unsigned long parsedPort = std::strtoul(port.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || parsedPort == 0UL || parsedPort > UINT16_MAX) return false;
        host.resize(colon);
    }

    if (host.empty() || host.front() == '.' || host.back() == '.') return false;
    std::transform(host.begin(), host.end(), host.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    normalized = std::move(host);
    return true;
}

inline bool isAllowedHost(const char *rawHost, const char *stationIp, const char *accessPointIp,
                          const char *hostname) {
    std::string host;
    if (!normalizeHost(rawHost, host)) return false;

    const auto matchesActiveIp = [&host](const char *ip) {
        return ip != nullptr && *ip != '\0' && std::string(ip) != "0.0.0.0" && host == ip;
    };
    if (matchesActiveIp(stationIp) || matchesActiveIp(accessPointIp)) return true;
    if (hostname == nullptr || *hostname == '\0') return false;

    std::string normalizedHostname;
    if (!normalizeHost(hostname, normalizedHostname)) return false;
    return host == normalizedHostname || host == normalizedHostname + ".local";
}

inline bool isAllowed(const char *marker, const char *host, const char *stationIp,
                      const char *accessPointIp, const char *hostname) {
    return hasValidMarker(marker) && isAllowedHost(host, stationIp, accessPointIp, hostname);
}

}  // namespace MutationRequestGuardCore

#endif
