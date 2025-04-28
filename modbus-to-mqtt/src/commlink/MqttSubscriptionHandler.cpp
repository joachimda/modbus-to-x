#include "commlink/MqttSubscriptionHandler.h"
#include <utility>

MqttSubscriptionHandler::MqttSubscriptionHandler(Logger *logger) : _logger(logger){}

void MqttSubscriptionHandler::addHandler(const char* topic, TopicHandlerFunc func) {
    HandlerEntry entry;
    entry.topic = topic;
    entry.handlerFunc = std::move(func);
    _handlers.push_back(entry);
    _logger->logInformation((String("Handler added for topic: [")+ topic + "]").c_str());
}

void MqttSubscriptionHandler::handle(const char* topic, const String& message) {
    for (auto &entry : _handlers) {
        _logger->logInformation(entry.topic.c_str());
        _logger->logInformation(topic);
        if (entry.topic.equals(topic)) {
            _logger->logDebug((String("MqttSubscriptionHandler::handle - Matched handler for topic [") + topic + "]").c_str());
            entry.handlerFunc(message);
            break;
        }
        else {
            _logger->logError((String("MqttSubscriptionHandler::handle - No matching handler for topic [") + topic + "]").c_str());
        }
    }

}