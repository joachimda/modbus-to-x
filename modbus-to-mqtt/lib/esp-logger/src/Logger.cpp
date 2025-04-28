#include "Logger.h"
void Logger::addTarget(LoggerInterface* target) {
    _targets.push_back(target);
}

void Logger::logInformation(const char *message) const {
    for (auto *target : _targets) {
        target->logInformation(message);
    }
}

void Logger::logWarning(const char *message) const {
    for (auto *target : _targets) {
        target->logWarning(message);
    }
}

void Logger::logError(const char *message) const {
    for (auto *target : _targets) {
        target->logError(message);
    }
}

void Logger::logDebug(const char *message) const {
    if (!_writeDebug) return;

    for (auto *target : _targets) {
        target->logDebug(message);
    }
}

void Logger::useDebug(const bool debugEnabled) {
    _writeDebug = debugEnabled;
}
