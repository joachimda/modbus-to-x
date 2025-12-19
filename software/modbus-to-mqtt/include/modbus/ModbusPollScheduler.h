#ifndef MODBUS_POLL_SCHEDULER_H
#define MODBUS_POLL_SCHEDULER_H

#include <vector>

#include "modbus/config_structs/ModbusDevice.h"
#include "modbus/config_structs/ModbusDatapoint.h"

class ModbusPollScheduler {
public:
    static bool isDue(const ModbusDatapoint &dp, uint32_t nowMs);

    static void scheduleNext(ModbusDatapoint &dp, uint32_t nowMs);

    static std::vector<ModbusDatapoint *> dueReadDatapoints(ModbusDevice &device, uint32_t nowMs);

    static bool hasDueReadDatapoints(const ModbusDevice &device, uint32_t nowMs);

    static size_t collectDueReadDatapoints(ModbusDevice &device,
                                           uint32_t nowMs,
                                           std::vector<ModbusDatapoint *> &out);
};

#endif
