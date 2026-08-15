#ifndef MQTTSUBSCRIPTIONHANDLER_H
#define MQTTSUBSCRIPTIONHANDLER_H

#include <WString.h>
#include <vector>
#include <functional>
#include <utility>

#include "Logger.h"

class MqttSubscriptionHandler {
public:
    explicit MqttSubscriptionHandler(Logger *logger);

    using TopicHandlerFunc = std::function<void(const String &)>;

    struct HandlerEntry {
        HandlerEntry() = default;
        HandlerEntry(String topicValue, TopicHandlerFunc handlerValue)
            : topic(std::move(topicValue)), handlerFunc(std::move(handlerValue)) {}

        String topic;
        TopicHandlerFunc handlerFunc;
    };

    std::vector<String> getHandlerTopics() const;

    void addHandler(const String &topic, TopicHandlerFunc handler);

    void removeHandlers(const std::vector<String> &topics);

    void clear();

    void replaceHandlers(std::vector<HandlerEntry> handlers);

    void handle(const String &topic, const String &message) const;

private:
    std::vector<HandlerEntry> _handlers;
    Logger *_logger;
};

#endif
