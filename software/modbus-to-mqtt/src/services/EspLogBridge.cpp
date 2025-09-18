#include "EspLogBridge.h"
#include <esp_log.h>
#include <Arduino.h>
#include "MemoryLogger.h"
#include "RtcLogBuffer.h"

namespace {
    static MemoryLogger *s_mem = nullptr;
    static vprintf_like_t s_prev = nullptr;

    // vprintf-like hook called by ESP_LOGx
    int hook_vprintf(const char *fmt, va_list ap) {
        char buf[256];
        va_list aq;
        va_copy(aq, ap);
        int n = vsnprintf(buf, sizeof(buf), fmt, aq);
        va_end(aq);
        // Mirror to memory logger (best effort)
        if (s_mem) {
            // avoid adding extra newlines; MemoryLogger stores whole lines
            // Strip trailing newlines for cleaner UI
            size_t len = strnlen(buf, sizeof(buf));
            while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
            if (len) s_mem->logDebug(buf);
        }
        // Mirror to RTC retained buffer as well
        RtcLogBuffer::appendLine(buf);
        // Forward to previous printer to keep serial output intact
        if (s_prev) return s_prev(fmt, ap);
        return n;
    }
}

namespace EspLogBridge {
    void begin(MemoryLogger *mem) {
        s_mem = mem;
        s_prev = esp_log_set_vprintf(&hook_vprintf);
        // Optional: bump log level if desired; leave as configured elsewhere
        // esp_log_level_set("*", ESP_LOG_INFO);
    }
}
