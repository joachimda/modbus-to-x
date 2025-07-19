#ifndef SERIALLOGGER_H
#define SERIALLOGGER_H
#pragma once

#include <Stream.h>
#include "LoggerInterface.h"
#include "LogLevel.h"

class SerialLogger final : public LoggerInterface {
public:
    explicit SerialLogger(Stream &stream);

    void logError(const char *message) override;

    void logInformation(const char *message) override;

    void logWarning(const char *message) override;

    void logDebug(const char *message) override;

private:
    Stream &_stream;

    static const char* levelPrefix(LogLevel level);
};
#endif //SERIALLOGGER_H
