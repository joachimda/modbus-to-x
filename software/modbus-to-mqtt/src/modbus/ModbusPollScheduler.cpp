#include "modbus/ModbusPollScheduler.h"

#include "modbus/ModbusFunctionUtils.h"

bool ModbusPollScheduler::isDue(const ModbusDatapoint &dp, const uint32_t nowMs) {
    if (!isReadOnlyFunction(dp.function)) {
        return false;
    }
    if (dp.pollIntervalMs == 0) {
        return true;
    }

    return nowMs >= dp.nextDueAtMs;
}

void ModbusPollScheduler::scheduleNext(ModbusDatapoint &dp, const uint32_t nowMs) {
    if (dp.pollIntervalMs > 0) {
        dp.nextDueAtMs = nowMs + dp.pollIntervalMs;
    } else {
        dp.nextDueAtMs = 0;
    }
}

std::vector<ModbusDatapoint *> ModbusPollScheduler::dueReadDatapoints(ModbusDevice &device, const uint32_t nowMs) {
    std::vector<ModbusDatapoint *> due;
    due.reserve(device.datapoints.size());
    collectDueReadDatapoints(device, nowMs, due);
    return due;
}

bool ModbusPollScheduler::hasDueReadDatapoints(const ModbusDevice &device, const uint32_t nowMs) {
    for (const auto &dp : device.datapoints) {
        if (isReadOnlyFunction(dp.function)) {
            if (dp.pollIntervalMs == 0 || nowMs >= dp.nextDueAtMs) {
                return true;
            }
        }
    }
    return false;
}

size_t ModbusPollScheduler::collectDueReadDatapoints(ModbusDevice &device,
                                                     const uint32_t nowMs,
                                                     std::vector<ModbusDatapoint *> &out) {
    for (auto &dp : device.datapoints) {
        if (isDue(dp, nowMs)) {
            out.push_back(&dp);
        }
    }
    return out.size();
}
