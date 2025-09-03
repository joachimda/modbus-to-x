#ifndef MQTTSUBSCRIPTIONHANDLER_H
#define MQTTSUBSCRIPTIONHANDLER_H

#include <WString.h>
#include <vector>
#include <functional>

#include "Logger.h"

class MqttSubscriptionHandler {
public:
    explicit MqttSubscriptionHandler(Logger *logger);

    using TopicHandlerFunc = std::function<void(const String &)>;

    std::vector<String> getHandlerTopics() const;

    void addHandler(const String &topic, TopicHandlerFunc handler);

    void handle(const String &topic, const String &message) const;

    struct HandlerEntry {
        String topic;
        TopicHandlerFunc handlerFunc;
    };

private:
    std::vector<HandlerEntry> _handlers;
    Logger *_logger;
};

#endif
