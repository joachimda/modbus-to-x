#include "logging/MemoryLogger.h"

#include <Print.h>
#include <algorithm>
#include <cstring>
#include "services/TimeService.h"

namespace {
    class MutexLock {
    public:
        explicit MutexLock(const SemaphoreHandle_t handle) : _handle(handle), _locked(false) {
            if (_handle) {
                _locked = xSemaphoreTake(_handle, portMAX_DELAY) == pdTRUE;
            }
        }

        ~MutexLock() {
            if (_locked) {
                xSemaphoreGive(_handle);
            }
        }

        MutexLock(const MutexLock &) = delete;

        MutexLock &operator=(const MutexLock &) = delete;

        bool locked() const { return _locked; }

    private:
        SemaphoreHandle_t _handle;
        bool _locked;
    };
}

MemoryLogger::MemoryLogger(const size_t maxLines) : _maxLines(maxLines) {
    _mutex = xSemaphoreCreateMutex();
}

MemoryLogger::~MemoryLogger() {
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

void MemoryLogger::setMaxLines(const size_t n) {
    MutexLock lock(_mutex);
    if (!lock.locked()) {
        _maxLines = n > 0 ? n : 1;
        return;
    }

    _maxLines = n > 0 ? n : 1;
    if (_lines.size() > _maxLines) {
        _lines.erase(_lines.begin(), _lines.begin() + (_lines.size() - _maxLines));
    }
}

size_t MemoryLogger::size() const {
    MutexLock lock(_mutex);
    if (!lock.locked()) {
        return 0;
    }
    return _lines.size();
}

void MemoryLogger::append(const char *level, const char *message) {
    String line;
    line.reserve(strlen(message) + 16);
    line += ts();
    line += " ";
    line += level;
    line += " ";
    line += message;

    MutexLock lock(_mutex);
    if (!lock.locked()) {
        return;
    }
    _lines.emplace_back(line);
    if (_lines.size() > _maxLines) {
        _lines.erase(_lines.begin(), _lines.begin() + (_lines.size() - _maxLines));
    }
}

void MemoryLogger::logError(const char *message) { append("[ERROR]", message); }
void MemoryLogger::logInformation(const char *message) { append("[INFO]", message); }
void MemoryLogger::logWarning(const char *message) { append("[WARN]", message); }
void MemoryLogger::logDebug(const char *message) { append("[DEBUG]", message); }

String MemoryLogger::ts() {
    if (TimeService::hasValidTime()) {
        const String iso = TimeService::nowIso();
        if (!iso.isEmpty()) return iso;
    }

    const uint32_t ms = millis();
    const uint32_t s = ms / 1000u;
    const uint32_t hh = (s / 3600u) % 24u;
    const uint32_t mm = (s / 60u) % 60u;
    const uint32_t ss = s % 60u;
    char buf[10];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hh, mm, ss);
    return {buf};
}

String MemoryLogger::toText() const {
    String out;
    MutexLock lock(_mutex);
    if (!lock.locked()) {
        return out;
    }
    for (size_t i = 0; i < _lines.size(); ++i) {
        out += _lines[i];
        if (i + 1 < _lines.size()) out += '\n';
    }
    return out;
}

std::vector<String> MemoryLogger::lines() const {
    std::vector<String> copy;
    MutexLock lock(_mutex);
    if (!lock.locked()) {
        return copy;
    }
    copy = _lines;
    return copy;
}

void MemoryLogger::streamTo(Print &out) const {
    MutexLock lock(_mutex);
    if (!lock.locked()) {
        return;
    }

    for (const auto &line: _lines) {
        out.print(line);
        out.print('\n');
    }
}

size_t MemoryLogger::flattenedSize() const {
    MutexLock lock(_mutex);
    if (!lock.locked()) {
        return 0;
    }
    size_t total = 0;
    for (size_t i = 0; i < _lines.size(); ++i) {
        total += _lines[i].length();
        if (i + 1 < _lines.size()) {
            ++total; // newline
        }
    }
    return total;
}

size_t MemoryLogger::copyAsText(size_t offset, uint8_t *dest, size_t maxLen) const {
    if (!dest || maxLen == 0) {
        return 0;
    }

    MutexLock lock(_mutex);
    if (!lock.locked()) {
        return 0;
    }

    size_t written = 0;
    size_t cursor = 0;
    const size_t count = _lines.size();

    for (size_t i = 0; i < count && written < maxLen; ++i) {
        const String &line = _lines[i];
        const size_t lineLen = line.length();

        if (offset < cursor + lineLen) {
            const size_t start = (offset > cursor) ? (offset - cursor) : 0;
            const size_t remaining = lineLen - start;
            const size_t toCopy = std::min(remaining, maxLen - written);
            if (toCopy > 0) {
                memcpy(dest + written, line.c_str() + start, toCopy);
                written += toCopy;
            }
        }
        cursor += lineLen;
        if (written >= maxLen) {
            break;
        }
        const bool hasNewline = (i + 1 < count);
        if (hasNewline && written < maxLen) {
            if (offset <= cursor && offset < cursor + 1) {
                dest[written++] = '\n';
            }
            cursor += 1;
        }
    }
    return written;
}
