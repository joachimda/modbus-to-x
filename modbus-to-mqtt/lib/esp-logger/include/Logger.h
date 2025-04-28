#pragma once
#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <vector>
#include "LoggerInterface.h"

class Logger {
public:
    void addTarget(LoggerInterface* target);

    void logInformation(const char *message) const;

    void logWarning(const char *message) const;

    void logError(const char *message) const;

    void logDebug(const char *message) const;

    void useDebug(bool debugEnabled);
private:
    std::vector<LoggerInterface*> _targets;
    bool _writeDebug = false;
};
#endif //LOGGER_H
