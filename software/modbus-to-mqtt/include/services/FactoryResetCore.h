#ifndef MODBUS_TO_MQTT_FACTORYRESETCORE_H
#define MODBUS_TO_MQTT_FACTORYRESETCORE_H

#include <cstdint>

/**
 * @file FactoryResetCore.h
 * @brief Platform-independent orchestration of factory-reset operations.
 */

/**
 * @brief Scheduling and ordered execution helpers for a factory reset.
 *
 * Platform-specific storage operations are supplied as callbacks, allowing the
 * control flow and failure handling to be tested without target hardware.
 */
namespace FactoryResetCore {

/** @brief Result of attempting to schedule a factory-reset task. */
enum class StartResult : std::uint8_t {
    /** The scheduler accepted responsibility for running the reset task. */
    Accepted,

    /** No scheduler was supplied or the task could not be scheduled. */
    TaskUnavailable,
};

/** @brief Result of executing the persistent-storage reset steps. */
enum class RunResult : std::uint8_t {
    /** Every reset step completed successfully. */
    Ok,

    /** Formatting configuration storage failed or its callback was missing. */
    ConfigStorageFailed,

    /** Erasing non-volatile storage failed or its callback was missing. */
    NvsStorageFailed,
};

/**
 * @brief Callback that performs one synchronous factory-reset step.
 *
 * @param[in,out] context Opaque caller-provided context.
 * @return `true` if the step completed successfully; otherwise `false`.
 */
using Step = bool (*)(void *context);

/**
 * @brief Callback that schedules asynchronous execution of a factory reset.
 *
 * @param[in,out] context Opaque context whose lifetime must extend through the
 *        scheduled work.
 * @return `true` if the task was scheduled; otherwise `false`.
 */
using Schedule = bool (*)(void *context);

/** @brief Platform-specific persistent-storage operations used by @ref execute. */
struct Operations {
    /** Formats the device's configuration storage. */
    Step formatConfigStorage;

    /** Erases the device's non-volatile key-value storage. */
    Step eraseNvsStorage;
};

/**
 * @brief Requests that a factory-reset task be scheduled.
 *
 * This function only delegates scheduling; it does not call @ref execute.
 *
 * @param[in] schedule Callback responsible for creating the reset task.
 * @param[in,out] context Opaque value passed to @p schedule.
 * @return @ref StartResult::Accepted if scheduling succeeds; otherwise
 *         @ref StartResult::TaskUnavailable.
 */
inline StartResult start(const Schedule schedule, void *context) {
    return schedule != nullptr && schedule(context) ? StartResult::Accepted : StartResult::TaskUnavailable;
}

/**
 * @brief Executes the persistent-storage reset steps in order.
 *
 * Configuration storage is formatted first. Non-volatile storage is erased
 * only if formatting succeeds. Execution stops at the first missing or failed
 * callback, and no rollback is attempted.
 *
 * @param[in] operations Reset callbacks to execute.
 * @param[in,out] context Opaque value passed unchanged to both callbacks.
 * @return @ref RunResult::Ok if both steps succeed, or the result identifying
 *         the first step that could not be completed.
 * @warning If non-volatile storage erasure fails, configuration storage has
 *          already been formatted and remains so.
 */
inline RunResult execute(const Operations &operations, void *context) {
    if (operations.formatConfigStorage == nullptr || !operations.formatConfigStorage(context)) {
        return RunResult::ConfigStorageFailed;
    }

    if (operations.eraseNvsStorage == nullptr || !operations.eraseNvsStorage(context)) {
        return RunResult::NvsStorageFailed;
    }

    return RunResult::Ok;
}

}  // namespace FactoryResetCore

#endif
