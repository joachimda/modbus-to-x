#ifndef TOPICHANDLERSTRUCT_H
#define TOPICHANDLERSTRUCT_H

#include <WString.h>
#include <vector>
#include <functional>

#include "Logger.h"

class MqttSubscriptionHandler {
public:
    explicit MqttSubscriptionHandler(Logger *logger);
    using TopicHandlerFunc = std::function<void(const String&)>;
    struct HandlerEntry {
        String topic;
        TopicHandlerFunc handlerFunc;
    };

    void addHandler(const char* topic, TopicHandlerFunc handler);
    void handle(const char* topic, const String& message);
private:
     std::vector<HandlerEntry> _handlers;
    Logger * _logger;
};

#endif //TOPICHANDLERSTRUCT_H