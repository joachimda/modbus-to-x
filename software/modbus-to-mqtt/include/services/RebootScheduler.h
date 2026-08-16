#ifndef MODBUS_TO_MQTT_REBOOTSCHEDULER_H
#define MODBUS_TO_MQTT_REBOOTSCHEDULER_H

#include <cstdint>

#include "services/RebootSchedulerCore.h"

class RebootScheduler {
public:
    using ScheduleResult = RebootSchedulerCore::ScheduleResult;

    static constexpr std::uint32_t RESPONSE_GRACE_MS = 1000U;

    static ScheduleResult schedule();
};

#endif
