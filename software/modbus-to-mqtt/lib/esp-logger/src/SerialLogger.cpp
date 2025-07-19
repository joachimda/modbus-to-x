#include "SerialLogger.h"
#include <WString.h>
#include "LogLevel.h"

SerialLogger::SerialLogger(Stream &stream) : _stream(stream) {
}

void SerialLogger::logError(const char *message) {
    const char *prefix = levelPrefix(LOGLEVEL_ERROR);
    _stream.println((String(prefix) + message).c_str());
}

void SerialLogger::logInformation(const char *message) {
    const char* prefix = levelPrefix(LOGLEVEL_INFO);
    _stream.println((String(prefix) + message).c_str());
}

void SerialLogger::logWarning(const char *message) {
    const char* prefix = levelPrefix(LOGLEVEL_WARN);
    _stream.println((String(prefix) + message).c_str());
}

void SerialLogger::logDebug(const char *message) {
    const char* prefix = levelPrefix(LOGLEVEL_DEBUG);
    _stream.println((String(prefix) + message).c_str());
}

const char * SerialLogger::levelPrefix(const LogLevel level) {
    switch (level) {
        case LOGLEVEL_DEBUG: return "[DEBUG] ";
        case LOGLEVEL_INFO:  return "[INFO] ";
        case LOGLEVEL_WARN:  return "[WARN] ";
        case LOGLEVEL_ERROR: return "[ERROR] ";
        default:             return "[UNKNOWN] ";
    }
}
