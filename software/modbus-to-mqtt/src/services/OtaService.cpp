#include "services/OtaService.h"
#include <Update.h>
#include "Logger.h"
#include "services/IndicatorService.h"

bool OtaService::beginFirmware(const size_t totalSize, const Logger *logger) {
    (void) totalSize;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        if (logger) logger->logError("OtaService::beginFirmware - Update.begin failed");
        return false;
    }
    if (logger) logger->logInformation("OtaService::beginFirmware - Update started");
    IndicatorService::instance().setOtaActive(true);
    return true;
}

bool OtaService::write(uint8_t *data, const size_t len, const Logger *logger) {
    const size_t written = Update.write(data, len);
    if (written != len) {
        if (logger) logger->logError("OtaService::write - short write");
        return false;
    }
    return true;
}

bool OtaService::end(const bool evenIfHasError, const Logger *logger) {
    const bool ok = Update.end(evenIfHasError);
    if (!ok || Update.hasError()) {
        if (logger) logger->logError("OtaService::end - Update failed");
        IndicatorService::instance().setOtaActive(false);
        return false;
    }
    if (logger) logger->logInformation("OtaService::end - Update successful");
    IndicatorService::instance().setOtaActive(false);
    return true;
}

bool OtaService::beginFilesystem(const size_t totalSize, const Logger *logger) {
    (void) totalSize;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
        if (logger) logger->logError("OtaService::beginFilesystem - Update.begin failed");
        return false;
    }
    if (logger) logger->logInformation("OtaService::beginFilesystem - Update started");
    IndicatorService::instance().setOtaActive(true);
    return true;
}