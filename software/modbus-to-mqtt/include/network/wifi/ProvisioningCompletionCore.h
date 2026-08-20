#ifndef MODBUS_TO_MQTT_PROVISIONINGCOMPLETIONCORE_H
#define MODBUS_TO_MQTT_PROVISIONINGCOMPLETIONCORE_H

#include <cstdint>

#include "Ipv4AddressCore.h"

namespace ProvisioningCompletionCore {

enum class ScheduleResult : std::uint8_t {
    Accepted,
    Pending,
    Unavailable,
};

enum class Result : std::uint8_t {
    NotReady,
    Accepted,
    Pending,
    Unavailable,
};

using Schedule = ScheduleResult (*)(void *context);

constexpr const char *NOT_READY_BODY = R"({"ok":false,"error":"provisioning_not_ready"})";
constexpr const char *PENDING_BODY = R"({"ok":false,"error":"reboot_pending"})";
constexpr const char *UNAVAILABLE_BODY = R"({"ok":false,"error":"reboot_unavailable"})";

template<typename Text>
Text acceptedBody(const Text &ip) {
    Text body(R"({"ok":true,"rebooting":true,"ip":")");
    body += ip;
    body += R"("})";
    return body;
}

template<typename Text>
Result complete(const bool connected, const Text &ip, const bool provisioningReady,
                const Schedule schedule, void *context) {
    if (!connected || !Ipv4AddressCore::isValidStationAddress(ip)
        || !provisioningReady || schedule == nullptr) {
        return Result::NotReady;
    }
    switch (schedule(context)) {
        case ScheduleResult::Accepted: return Result::Accepted;
        case ScheduleResult::Pending: return Result::Pending;
        case ScheduleResult::Unavailable: return Result::Unavailable;
    }
    return Result::Unavailable;
}

}  // namespace ProvisioningCompletionCore

#endif
