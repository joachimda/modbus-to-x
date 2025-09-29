#ifndef MQTTLOGGER_H
#define MQTTLOGGER_H
#pragma once
#include "LoggerInterface.h"
#include <functional>
#include "LogLevel.h"

class MqttLogger final : public LoggerInterface {
public:
    explicit MqttLogger(std::function<void(const char*)> mqttSendFunc);

    void logError(const char *message) override;

    void logInformation(const char *message) override;

    void logWarning(const char *message) override;

    void logDebug(const char *message) override;

private:
    std::function<void(const char*)> _mqttSendFunc;

    static const char* getLevelPrefix(LogLevel level);
};

#endif
