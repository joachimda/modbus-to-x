#ifndef MODBUS_TO_MQTT_REBOOTSCHEDULERCORE_H
#define MODBUS_TO_MQTT_REBOOTSCHEDULERCORE_H

#include <atomic>
#include <cstdint>

namespace RebootSchedulerCore {

enum class ScheduleResult : std::uint8_t {
    Accepted,
    Pending,
    Unavailable,
};

using Task = void (*)(void *taskContext);
using ScheduleTask = bool (*)(Task task, void *taskContext, void *scheduleContext);
using Wait = void (*)(std::uint32_t delayMs, void *executionContext);
using Restart = void (*)(void *executionContext);

struct Operations {
    ScheduleTask scheduleTask;
    void *scheduleContext;
    Wait wait;
    Restart restart;
    void *executionContext;
    std::uint32_t gracePeriodMs;
};

class Scheduler {
public:
    ScheduleResult schedule(const Operations &operations) {
        if (operations.scheduleTask == nullptr || operations.wait == nullptr
            || operations.restart == nullptr) {
            return ScheduleResult::Unavailable;
        }

        bool expected = false;
        if (!_pending.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return ScheduleResult::Pending;
        }

        _wait = operations.wait;
        _restart = operations.restart;
        _executionContext = operations.executionContext;
        _gracePeriodMs = operations.gracePeriodMs;
        std::atomic_thread_fence(std::memory_order_release);

        if (!operations.scheduleTask(taskRunner, this, operations.scheduleContext)) {
            _pending.store(false, std::memory_order_release);
            return ScheduleResult::Unavailable;
        }

        return ScheduleResult::Accepted;
    }

    bool isPending() const {
        return _pending.load(std::memory_order_acquire);
    }

private:
    static void taskRunner(void *context) {
        auto *scheduler = static_cast<Scheduler *>(context);
        std::atomic_thread_fence(std::memory_order_acquire);
        scheduler->_wait(scheduler->_gracePeriodMs, scheduler->_executionContext);
        scheduler->_restart(scheduler->_executionContext);
    }

    std::atomic<bool> _pending{false};
    Wait _wait = nullptr;
    Restart _restart = nullptr;
    void *_executionContext = nullptr;
    std::uint32_t _gracePeriodMs = 0U;
};

}  // namespace RebootSchedulerCore

#endif
