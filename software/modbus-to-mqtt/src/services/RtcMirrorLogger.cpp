#include "RtcMirrorLogger.h"
#include "RtcLogBuffer.h"
#include <Arduino.h>

namespace {
    static String ts() {
        const uint32_t ms = millis();
        const uint32_t s = ms / 1000u;
        const uint32_t hh = (s / 3600u) % 24u;
        const uint32_t mm = (s / 60u) % 60u;
        const uint32_t ss = s % 60u;
        char buf[10];
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hh, mm, ss);
        return {buf};
    }
}

void RtcMirrorLogger::append(const char *level, const char *message) {
    // format a single line similar to MemoryLogger
    String line;
    line.reserve(strlen(message) + 16);
    line += ts();
    line += " ";
    line += level;
    line += " ";
    line += message;
    RtcLogBuffer::appendLine(line.c_str());
}

void RtcMirrorLogger::logError(const char *message) { append("[ERROR]", message); }
void RtcMirrorLogger::logInformation(const char *message) { append("[INFO]", message); }
void RtcMirrorLogger::logWarning(const char *message) { append("[WARN]", message); }
void RtcMirrorLogger::logDebug(const char *message) { append("[DEBUG]", message); }

