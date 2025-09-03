#ifndef LOGGERINTERFACE_H
#define LOGGERINTERFACE_H

class LoggerInterface {
public:
    virtual ~LoggerInterface() = default;
    virtual void logError(const char* message) = 0;
    virtual void logInformation(const char* message) = 0;
    virtual void logWarning(const char* message) = 0;
    virtual void logDebug(const char* message) = 0;
};
#endif
