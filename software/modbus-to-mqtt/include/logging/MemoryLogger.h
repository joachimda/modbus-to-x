#ifndef MEMORYLOGGER_H
#define MEMORYLOGGER_H
#pragma once

#include <Arduino.h>
#include <vector>
#include <freertos/semphr.h>
#include "LoggerInterface.h"

class Print;

class MemoryLogger final : public LoggerInterface {
public:
    explicit MemoryLogger(size_t maxLines = 200);
    ~MemoryLogger();

    void logError(const char *message) override;
    void logInformation(const char *message) override;
    void logWarning(const char *message) override;
    void logDebug(const char *message) override;

    void setMaxLines(size_t n);
    size_t size() const;
    String toText() const;
    std::vector<String> lines() const;
    void streamTo(Print &out) const;
    size_t flattenedSize() const;
    size_t copyAsText(size_t offset, uint8_t *dest, size_t maxLen) const;

private:
    void append(const char* level, const char* message);
    static String ts();

    mutable SemaphoreHandle_t _mutex = nullptr;
    size_t _maxLines;
    std::vector<String> _lines;
};

#endif // MEMORYLOGGER_H

