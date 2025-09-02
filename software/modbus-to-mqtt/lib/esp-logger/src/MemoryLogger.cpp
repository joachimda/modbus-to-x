#include "MemoryLogger.h"

MemoryLogger::MemoryLogger(size_t maxLines) : _maxLines(maxLines) {}

void MemoryLogger::setMaxLines(size_t n) {
    portENTER_CRITICAL(&_mux);
    _maxLines = n > 0 ? n : 1;
    if (_lines.size() > _maxLines) {
        _lines.erase(_lines.begin(), _lines.begin() + (_lines.size() - _maxLines));
    }
    portEXIT_CRITICAL(&_mux);
}

size_t MemoryLogger::size() const {
    portENTER_CRITICAL(&const_cast<MemoryLogger*>(this)->_mux);
    const size_t s = _lines.size();
    portEXIT_CRITICAL(&const_cast<MemoryLogger*>(this)->_mux);
    return s;
}

void MemoryLogger::append(const char *level, const char *message) {
    String line;
    line.reserve(strlen(message) + 16);
    line += ts();
    line += " ";
    line += level;
    line += " ";
    line += message;

    portENTER_CRITICAL(&_mux);
    _lines.emplace_back(line);
    if (_lines.size() > _maxLines) {
        _lines.erase(_lines.begin(), _lines.begin() + (_lines.size() - _maxLines));
    }
    portEXIT_CRITICAL(&_mux);
}

void MemoryLogger::logError(const char *message) { append("[ERROR]", message); }
void MemoryLogger::logInformation(const char *message) { append("[INFO]", message); }
void MemoryLogger::logWarning(const char *message) { append("[WARN]", message); }
void MemoryLogger::logDebug(const char *message) { append("[DEBUG]", message); }

String MemoryLogger::ts() {
    // Basic HH:MM:SS from millis
    const uint32_t ms = millis();
    const uint32_t s = ms / 1000u;
    const uint32_t hh = (s / 3600u) % 24u;
    const uint32_t mm = (s / 60u) % 60u;
    const uint32_t ss = s % 60u;
    char buf[10];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hh, mm, ss);
    return String(buf);
}

String MemoryLogger::toText() const {
    String out;
    portENTER_CRITICAL(&const_cast<MemoryLogger*>(this)->_mux);
    for (size_t i = 0; i < _lines.size(); ++i) {
        out += _lines[i];
        if (i + 1 < _lines.size()) out += '\n';
    }
    portEXIT_CRITICAL(&const_cast<MemoryLogger*>(this)->_mux);
    return out;
}

std::vector<String> MemoryLogger::lines() const {
    std::vector<String> copy;
    portENTER_CRITICAL(&const_cast<MemoryLogger*>(this)->_mux);
    copy = _lines;
    portEXIT_CRITICAL(&const_cast<MemoryLogger*>(this)->_mux);
    return copy;
}