#include "services/OtaService.h"
#include <Update.h>
#include "Logger.h"
#include "services/IndicatorService.h"
#include "services/ota/OtaCoordinator.h"

namespace {

uint32_t g_operationId = 0U;
uint32_t g_receivedBytes = 0U;
OtaProgressCore::Component g_component = OtaProgressCore::Component::None;

}

bool OtaService::beginFirmware(const size_t totalSize, const uint32_t operationId, const Logger *logger) {
    if (totalSize == 0U || totalSize > UINT32_MAX
        || !OtaCoordinator::beginComponent(operationId, OtaProgressCore::Component::Firmware,
                                           static_cast<uint32_t>(totalSize))) return false;
    if (!Update.begin(totalSize, U_FLASH)) {
        if (logger) logger->logError("OtaService::beginFirmware - Update.begin failed");
        return false;
    }
    if (logger) logger->logInformation("OtaService::beginFirmware - Update started");
    g_operationId = operationId;
    g_receivedBytes = 0U;
    g_component = OtaProgressCore::Component::Firmware;
    return true;
}

bool OtaService::write(const uint32_t operationId, uint8_t *data, const size_t len, const Logger *logger) {
    if (operationId == 0U || operationId != g_operationId
        || g_component == OtaProgressCore::Component::None
        || !OtaCoordinator::noteManualUploadActivity(operationId)) return false;
    const size_t written = Update.write(data, len);
    if (written != len) {
        if (logger) logger->logError("OtaService::write - short write");
        return false;
    }
    const uint64_t received = static_cast<uint64_t>(g_receivedBytes) + written;
    g_receivedBytes = received > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(received);
    (void)OtaCoordinator::updateProgress(operationId, g_receivedBytes);
    return true;
}

bool OtaService::end(const uint32_t operationId, const bool evenIfHasError, const Logger *logger) {
    if (operationId == 0U || operationId != g_operationId
        || g_component == OtaProgressCore::Component::None
        || !OtaCoordinator::beginVerification(operationId, g_component)) return false;
    const bool ok = Update.end(evenIfHasError);
    if (!ok || Update.hasError()) {
        if (logger) logger->logError("OtaService::end - Update failed");
        return false;
    }
    if (logger) logger->logInformation("OtaService::end - Update successful");
    const OtaProgressCore::Component completed = g_component;
    g_operationId = 0U;
    g_receivedBytes = 0U;
    g_component = OtaProgressCore::Component::None;
    (void)OtaCoordinator::completeComponent(operationId, completed);
    return true;
}

bool OtaService::beginFilesystem(const size_t totalSize, const uint32_t operationId, const Logger *logger) {
    if (totalSize == 0U || totalSize > UINT32_MAX
        || !OtaCoordinator::beginComponent(operationId, OtaProgressCore::Component::Filesystem,
                                           static_cast<uint32_t>(totalSize))) return false;
    // The partition table contains two data/spiffs entries (cfg and spiffs);
    // without an explicit label the Update library targets the first one (cfg),
    // which is too small for the UI image. Force the spiffs partition.
    if (!Update.begin(totalSize, U_SPIFFS, -1, LOW, "spiffs")) {
        if (logger) logger->logError("OtaService::beginFilesystem - Update.begin failed");
        return false;
    }
    if (logger) logger->logInformation("OtaService::beginFilesystem - Update started");
    g_operationId = operationId;
    g_receivedBytes = 0U;
    g_component = OtaProgressCore::Component::Filesystem;
    return true;
}
