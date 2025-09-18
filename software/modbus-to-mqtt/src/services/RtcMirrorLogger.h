#pragma once

#include "LoggerInterface.h"

// Logger target that mirrors lines to RTC retained buffer
class RtcMirrorLogger final : public LoggerInterface {
public:
    void logError(const char *message) override;
    void logInformation(const char *message) override;
    void logWarning(const char *message) override;
    void logDebug(const char *message) override;

private:
    void append(const char *level, const char *message);
};

