#include "MqttLogger.h"
#include <WString.h>

MqttLogger::MqttLogger(std::function<void(const char *)> mqttSendFunc) : _mqttSendFunc(std::move(mqttSendFunc)) {
}

void MqttLogger::logError(const char *message) {
    const char *prefix = getLevelPrefix(LOGLEVEL_ERROR);
    const String fullMessage = String(prefix) + message;
    _mqttSendFunc(fullMessage.c_str());
}

void MqttLogger::logInformation(const char *message) {
    const char *prefix = getLevelPrefix(LOGLEVEL_INFO);
    const String fullMessage = String(prefix) + message;
    _mqttSendFunc(fullMessage.c_str());
}

void MqttLogger::logWarning(const char *message) {
    const char* prefix = getLevelPrefix(LOGLEVEL_WARN);
    const String fullMessage = String(prefix) + message;
    _mqttSendFunc(fullMessage.c_str());
}

void MqttLogger::logDebug(const char *message) {
    const char* prefix = getLevelPrefix(LOGLEVEL_DEBUG);
    const String fullMessage = String(prefix) + message;
    _mqttSendFunc(fullMessage.c_str());
}

const char * MqttLogger::getLevelPrefix(const LogLevel level) {
    switch (level) {
        case LOGLEVEL_DEBUG: return "[DEBUG] ";
        case LOGLEVEL_INFO:  return "[INFO] ";
        case LOGLEVEL_WARN:  return "[WARN] ";
        case LOGLEVEL_ERROR: return "[ERROR] ";
        default:             return "[UNKNOWN] ";
    }
}
