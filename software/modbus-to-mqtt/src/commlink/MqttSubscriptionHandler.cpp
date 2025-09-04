#include "commlink/MqttSubscriptionHandler.h"
#include <utility>

MqttSubscriptionHandler::MqttSubscriptionHandler(Logger *logger) : _logger(logger){}

std::vector<String> MqttSubscriptionHandler::getHandlerTopics() const {
    std::vector<String> topics;
    for (const auto& handler : _handlers) {
        topics.push_back(handler.topic);
    }
    return topics;
}

void MqttSubscriptionHandler::addHandler(const String& topic, TopicHandlerFunc handler) {
    HandlerEntry entry;
    entry.topic = topic;
    entry.handlerFunc = std::move(handler);
    _handlers.push_back(entry);
    _logger->logInformation((String("Handler added for topic: [")+ topic + "]").c_str());
}

void MqttSubscriptionHandler::handle(const String& topic, const String& message) const {
    auto foundHandler = false;
    for (auto &entry : _handlers) {
        if (entry.topic.equals(topic)) {
            _logger->logDebug((String("MqttSubscriptionHandler::handle - Matched handler for topic [") + topic + "]").c_str());
            entry.handlerFunc(message);
            foundHandler = true;
            break;
        }
    }
    if (!foundHandler) {
        _logger->logWarning((String("MqttSubscriptionHandler::handle - No handler found for topic [") + topic + "]").c_str());
    }
}

void MqttSubscriptionHandler::clear() {
    _handlers.clear();
    _logger->logInformation("MqttSubscriptionHandler::clear - cleared all handlers");
}
