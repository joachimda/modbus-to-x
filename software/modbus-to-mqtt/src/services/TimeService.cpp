#include "services/TimeService.h"

#include <time.h>
#include <atomic>

namespace {
constexpr time_t TIME_VALID_THRESHOLD = 1600000000; // ~2020-09-13
constexpr uint32_t TIME_SYNC_TIMEOUT_MS = 20000;
constexpr uint32_t TIME_SYNC_POLL_MS = 500;

std::atomic<bool> g_timeValid{false};
bool g_syncInFlight = false;
uint32_t g_syncStartedMs = 0;
uint32_t g_lastPollMs = 0;
} // namespace

void TimeService::requestSync() {
    if (g_timeValid.load(std::memory_order_relaxed)) {
        return;
    }
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    g_syncInFlight = true;
    g_syncStartedMs = millis();
    g_lastPollMs = 0;
}

void TimeService::loop() {
    if (!g_syncInFlight || g_timeValid.load(std::memory_order_relaxed)) {
        return;
    }
    const uint32_t nowMs = millis();
    if (g_lastPollMs && (nowMs - g_lastPollMs) < TIME_SYNC_POLL_MS) {
        return;
    }
    g_lastPollMs = nowMs;

    const time_t now = time(nullptr);
    if (now >= TIME_VALID_THRESHOLD) {
        g_timeValid.store(true, std::memory_order_relaxed);
        g_syncInFlight = false;
        return;
    }

    if (g_syncStartedMs && (nowMs - g_syncStartedMs) > TIME_SYNC_TIMEOUT_MS) {
        g_syncInFlight = false;
    }
}

bool TimeService::hasValidTime() {
    return g_timeValid.load(std::memory_order_relaxed);
}

String TimeService::formatIso(const time_t t) {
    struct tm tm{};
    gmtime_r(&t, &tm);
    char buf[25];
    const size_t n = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    if (n == 0) return "";
    return {buf};
}

String TimeService::nowIso() {
    const time_t now = time(nullptr);
    if (now < TIME_VALID_THRESHOLD) return "";
    return formatIso(now);
}
