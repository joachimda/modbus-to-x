#ifndef LOGTARGET_H
#define LOGTARGET_H

class LoggerInterface {
public:
    virtual ~LoggerInterface() = default;
    virtual void logError(const char* message) = 0;
    virtual void logInformation(const char* message) = 0;
    virtual void logWarning(const char* message) = 0;
    virtual void logDebug(const char* message) = 0;
};
#endif //LOGTARGET_H
