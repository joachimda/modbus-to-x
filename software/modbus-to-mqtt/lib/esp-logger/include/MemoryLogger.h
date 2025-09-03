#ifndef MEMORYLOGGER_H
#define MEMORYLOGGER_H
#pragma once

#include <Arduino.h>
#include <vector>
#include "LoggerInterface.h"

class MemoryLogger final : public LoggerInterface {
public:
    explicit MemoryLogger(size_t maxLines = 200);

    void logError(const char *message) override;
    void logInformation(const char *message) override;
    void logWarning(const char *message) override;
    void logDebug(const char *message) override;

    void setMaxLines(size_t n);
    size_t size() const;
    String toText() const;
    std::vector<String> lines() const;

private:
    void append(const char* level, const char* message);
    static String ts();

    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED ;
    size_t _maxLines;
    std::vector<String> _lines;
};

#endif // MEMORYLOGGER_H

