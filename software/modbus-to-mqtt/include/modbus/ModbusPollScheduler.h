#ifndef MODBUS_POLL_SCHEDULER_H
#define MODBUS_POLL_SCHEDULER_H

#include <vector>

#include "modbus/config_structs/ModbusDevice.h"
#include "modbus/config_structs/ModbusDatapoint.h"

class ModbusPollScheduler {
public:
    // Signed modular-time comparisons are unambiguous only while a deadline is
    // at most half of the uint32_t range away from the supplied timestamp.
    static constexpr uint32_t MAX_POLL_INTERVAL_MS = 0x7FFFFFFFU;
    static constexpr uint32_t MAX_LEGACY_POLL_INTERVAL_SECONDS =
        MAX_POLL_INTERVAL_MS / 1000U;

    static bool isDeadlineReached(uint32_t nowMs, uint32_t deadlineMs);

    static bool isDue(const ModbusDatapoint &dp, uint32_t nowMs);

    static void scheduleNext(ModbusDatapoint &dp, uint32_t nowMs);

    static std::vector<ModbusDatapoint *> dueReadDatapoints(ModbusDevice &device, uint32_t nowMs);

    static bool hasDueReadDatapoints(const ModbusDevice &device, uint32_t nowMs);

    static size_t collectDueReadDatapoints(ModbusDevice &device,
                                           uint32_t nowMs,
                                           std::vector<ModbusDatapoint *> &out);
};

#endif
