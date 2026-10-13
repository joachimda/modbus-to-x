#ifndef MODBUS_TO_MQTT_OTA_COORDINATOR_H
#define MODBUS_TO_MQTT_OTA_COORDINATOR_H

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

#include "services/ota/OtaProgressCore.h"

class Logger;
class MqttManager;
class ModbusManager;

class OtaCoordinator {
public:
    enum class Admission : uint8_t {
        Accepted = 0,
        Busy,
        Unavailable,
    };

    static void begin(Logger *logger, MqttManager *mqtt, ModbusManager *modbus);
    static void loop();

    static Admission reserveSigned(const char *targetVersion, bool firmwareSelected,
                                   bool filesystemSelected, uint32_t &operationId);
    static Admission reserveManual(OtaProgressCore::Component component, uint32_t totalBytes,
                                   uint32_t &operationId);
    static bool quiesce(uint32_t operationId, bool awaitManualUpload);

    static bool validateManualUpload(uint32_t operationId, OtaProgressCore::Component component,
                                     uint32_t &declaredSize);
    static bool beginComponent(uint32_t operationId, OtaProgressCore::Component component,
                               uint32_t totalBytes);
    static bool noteManualUploadActivity(uint32_t operationId);
    static bool updateProgress(uint32_t operationId, uint32_t receivedBytes);
    static bool beginVerification(uint32_t operationId, OtaProgressCore::Component component);
    static bool completeComponent(uint32_t operationId, OtaProgressCore::Component component);
    static bool succeed(uint32_t operationId);
    static void fail(uint32_t operationId, const char *errorCode);
    static bool failAsync(uint32_t operationId, const char *errorCode);

    static OtaProgressCore::Snapshot snapshot();
    static bool installationActive();
    static bool ordinaryHttpAllowed();
    static bool uiFilesystemAllowed();

    static bool createSession(char tokenHex[65]);
    static bool authorizeSession(const char *tokenHex, size_t length);
    static void revokeSessions();

private:
    static Admission reserve(const char *targetVersion, bool firmwareSelected,
                             bool filesystemSelected, bool manualSingleComponent,
                             uint32_t manualTotal, uint32_t &operationId);
};

#endif
