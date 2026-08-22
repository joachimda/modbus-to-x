#include "mqtt/MqttSubscriptionHandler.h"
#include <utility>
#include <algorithm>

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

void MqttSubscriptionHandler::removeHandlers(const std::vector<String> &topics) {
    if (topics.empty()) return;
    _handlers.erase(
        std::remove_if(_handlers.begin(), _handlers.end(), [&topics](const HandlerEntry &entry) {
            for (const auto &t : topics) {
                if (entry.topic == t) return true;
            }
            return false;
        }),
        _handlers.end());
}

void MqttSubscriptionHandler::handle(const String& topic, const String& message) const {
    TopicHandlerFunc selected;
    for (const auto &entry : _handlers) {
        if (entry.topic.equals(topic)) {
            _logger->logDebug((String("MqttSubscriptionHandler::handle - Matched handler for topic [") + topic + "]").c_str());
            selected = entry.handlerFunc;
            break;
        }
    }
    // Dispatch happens outside iteration. In the owner model mutations cannot
    // run concurrently, but copying also guarantees the callable remains alive
    // if a handler submits a replacement command for the next owner cycle.
    if (selected) {
        selected(topic, message);
    } else {
        _logger->logWarning((String("MqttSubscriptionHandler::handle - No handler found for topic [") + topic + "]").c_str());
    }
}

void MqttSubscriptionHandler::clear() {
    _handlers.clear();
    _logger->logInformation("MqttSubscriptionHandler::clear - cleared all handlers");
}

void MqttSubscriptionHandler::replaceHandlers(std::vector<HandlerEntry> handlers) {
    _handlers = std::move(handlers);
    _logger->logInformation(
        (String("MqttSubscriptionHandler::replaceHandlers - installed ") + String(_handlers.size())
         + " handlers").c_str());
}
