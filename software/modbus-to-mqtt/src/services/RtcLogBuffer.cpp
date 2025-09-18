#include "RtcLogBuffer.h"
#include <esp_system.h>

// Keep small to reduce RTC usage; stores plain text lines
// Use RTC_NOINIT so content survives soft resets (including panic)
// but not power cycles.
// Keep conservative to avoid overflowing RTC slow segment.
// Adjust if your project has more headroom.
constexpr size_t RTC_LOG_BUF_SIZE = 4096; // 4 KB
constexpr size_t RTC_LOG_MAX_LINE = 240;  // safety cap per line

// Buffer state in RTC retained memory
RTC_NOINIT_ATTR static char s_rtc_buf[RTC_LOG_BUF_SIZE];
RTC_NOINIT_ATTR static uint32_t s_rtc_len = 0;   // number of valid bytes
RTC_NOINIT_ATTR static uint32_t s_rtc_dirty = 0; // 0 = empty/clean, 1 = has data

// lightweight spinlock
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

namespace {
    void append_bytes(const char *data, size_t len) {
        if (len == 0) return;
        // Protect against corrupted state
        if (s_rtc_len > RTC_LOG_BUF_SIZE) s_rtc_len = 0;

        // If incoming chunk exceeds buffer, keep only the tail
        if (len >= RTC_LOG_BUF_SIZE) {
            data = data + (len - (RTC_LOG_BUF_SIZE - 1));
            len = RTC_LOG_BUF_SIZE - 1;
        }

        // Ensure space: drop oldest from head
        if (s_rtc_len + len >= RTC_LOG_BUF_SIZE) {
            const size_t drop = (s_rtc_len + len) - (RTC_LOG_BUF_SIZE - 1);
            if (drop < s_rtc_len) {
                memmove(s_rtc_buf, s_rtc_buf + drop, s_rtc_len - drop);
                s_rtc_len -= drop;
            } else {
                s_rtc_len = 0;
            }
        }

        memcpy(s_rtc_buf + s_rtc_len, data, len);
        s_rtc_len += static_cast<uint32_t>(len);
        s_rtc_buf[s_rtc_len] = '\0';
        s_rtc_dirty = 1;
    }
}

namespace RtcLogBuffer {
    bool hasData() {
        return s_rtc_dirty != 0 && s_rtc_len > 0;
    }

    void appendLine(const char *line) {
        if (!line) return;
        // best-effort; keep very simple, avoid dynamic allocation
        char tmp[RTC_LOG_MAX_LINE + 2];
        size_t n = strnlen(line, RTC_LOG_MAX_LINE);
        memcpy(tmp, line, n);
        // enforce single line
        for (size_t i = 0; i < n; ++i) {
            if (tmp[i] == '\r' || tmp[i] == '\n') tmp[i] = ' ';
        }
        tmp[n++] = '\n';
        tmp[n] = '\0';

        portENTER_CRITICAL(&s_mux);
        append_bytes(tmp, n);
        portEXIT_CRITICAL(&s_mux);
    }

    void drain(LineSink sink, void *user) {
        if (!hasData() || !sink) { s_rtc_len = 0; s_rtc_dirty = 0; return; }
        // Copy out to RAM (regular) so we can parse safely
        char *copy = (char *) malloc(s_rtc_len + 1);
        if (!copy) {
            // If allocation fails, just clear to avoid infinite growth
            s_rtc_len = 0; s_rtc_dirty = 0; return;
        }

        portENTER_CRITICAL(&s_mux);
        memcpy(copy, s_rtc_buf, s_rtc_len);
        const uint32_t len = s_rtc_len;
        s_rtc_len = 0;
        s_rtc_dirty = 0;
        s_rtc_buf[0] = '\0';
        portEXIT_CRITICAL(&s_mux);

        // Split by '\n' and emit lines
        uint32_t start = 0;
        for (uint32_t i = 0; i < len; ++i) {
            if (copy[i] == '\n') {
                copy[i] = '\0';
                if (i > start) sink(copy + start, user);
                start = i + 1;
            }
        }
        // trailing part (no newline)
        if (start < len) sink(copy + start, user);

        free(copy);
    }

    void clear() {
        portENTER_CRITICAL(&s_mux);
        s_rtc_len = 0;
        s_rtc_dirty = 0;
        s_rtc_buf[0] = '\0';
        portEXIT_CRITICAL(&s_mux);
    }
}
