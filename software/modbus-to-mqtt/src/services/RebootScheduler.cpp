#include "services/RebootScheduler.h"

#include <Arduino.h>

#include "Config.h"

namespace {

RebootSchedulerCore::Scheduler g_rebootScheduler;

bool scheduleTask(const RebootSchedulerCore::Task task, void *taskContext, void *) {
    return xTaskCreatePinnedToCore(
               task, "reboot", 2048, taskContext, 1, nullptr, APP_CPU_NUM) == pdPASS;
}

void waitForResponse(const std::uint32_t delayMs, void *) {
    vTaskDelay(pdMS_TO_TICKS(delayMs));
}

void restartDevice(void *) {
    ESP.restart();
    vTaskDelete(nullptr);
}

}  // namespace

RebootScheduler::ScheduleResult RebootScheduler::schedule() {
    const RebootSchedulerCore::Operations operations{
        scheduleTask,
        nullptr,
        waitForResponse,
        restartDevice,
        nullptr,
        RESPONSE_GRACE_MS,
    };
    return g_rebootScheduler.schedule(operations);
}
