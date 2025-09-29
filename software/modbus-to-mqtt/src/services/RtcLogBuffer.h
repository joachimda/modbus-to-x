// Lightweight RTC retained ring buffer for last-session logs
#pragma once

#include <Arduino.h>

namespace RtcLogBuffer {
    // Returns true if there is buffered data from previous session
    bool hasData();

    // Append a single line (truncated if needed). Newline appended automatically.
    void appendLine(const char *line);

    // Drain buffered lines to a callback and clear
    // The callback will be called once per line with null-terminated string
    typedef void (*LineSink)(const char *line, void *user);
    void drain(LineSink sink, void *user);

    // Clear buffer explicitly
    void clear();
}

