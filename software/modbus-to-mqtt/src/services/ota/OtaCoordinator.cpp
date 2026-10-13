#include "services/ota/OtaCoordinator.h"

#include <SPIFFS.h>
#include <Update.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "Config.h"
#include "Logger.h"
#include "modbus/ModbusManager.h"
#include "mqtt/MqttManager.h"
#include "network/mbx_server/MBXServerHandlers.h"
#include "services/IndicatorService.h"
#include "services/RebootScheduler.h"
#include "services/ota/OtaSessionCore.h"
#include "services/ota/OtaQuiescenceCore.h"

namespace {

portMUX_TYPE g_otaLock = portMUX_INITIALIZER_UNLOCKED;
OtaProgressCore::Lifecycle g_lifecycle;
OtaSessionCore::Store<2U> g_sessions(
    OTA_SESSION_ABSOLUTE_LIFETIME_MS, OTA_SESSION_IDLE_LIFETIME_MS);
Logger *g_logger = nullptr;
MqttManager *g_mqtt = nullptr;
ModbusManager *g_modbus = nullptr;
OtaProgressCore::Component g_manualComponent = OtaProgressCore::Component::None;
uint32_t g_manualTotal = 0U;
uint32_t g_awaitingUploadSince = 0U;
bool g_manualUploadTimerArmed = false;
bool g_filesystemUnmounted = false;
std::atomic<bool> g_recoveryTaskActive{false};

void logError(const char *message) {
    if (g_logger != nullptr) g_logger->logError(message);
}

void logInformation(const char *message) {
    if (g_logger != nullptr) g_logger->logInformation(message);
}

bool operationMatches(const uint32_t operationId) {
    portENTER_CRITICAL(&g_otaLock);
    const bool matches = g_lifecycle.active() && g_lifecycle.operationId() == operationId;
    portEXIT_CRITICAL(&g_otaLock);
    return matches;
}

bool ownersAcknowledged(const uint32_t operationId, bool &mqttReady, bool &modbusReady) {
    mqttReady = false;
    modbusReady = false;
    if (g_mqtt != nullptr) {
        const MqttStatusSnapshot mqtt = g_mqtt->getStatusSnapshot();
        mqttReady = mqtt.otaQuiesced && mqtt.otaOperationId == operationId;
    }
    if (g_modbus != nullptr) {
        const ModbusRuntimeSnapshot modbus = g_modbus->getRuntimeSnapshot();
        modbusReady = modbus.otaQuiesced && modbus.otaOperationId == operationId;
    }
    return mqttReady && modbusReady;
}

struct FailureContext {
    uint32_t operationId;
    char errorCode[OtaProgressCore::ERROR_CODE_CAPACITY];
};

FailureContext g_failureContext{};
std::atomic<bool> g_recoveryPending{false};

}  // namespace

void OtaCoordinator::begin(Logger *logger, MqttManager *mqtt, ModbusManager *modbus) {
    g_logger = logger;
    g_mqtt = mqtt;
    g_modbus = modbus;
}

auto OtaCoordinator::reserve(const char *targetVersion, const bool firmwareSelected,
                             const bool filesystemSelected, const bool manualSingleComponent,
                             const uint32_t manualTotal, uint32_t &operationId) -> Admission {
    operationId = 0U;
    if (g_logger == nullptr || g_mqtt == nullptr || g_modbus == nullptr
        || (!firmwareSelected && !filesystemSelected)) return Admission::Unavailable;
    portENTER_CRITICAL(&g_otaLock);
    const auto admission = g_lifecycle.reserve(
        targetVersion, firmwareSelected, filesystemSelected, manualSingleComponent);
    if (admission == OtaProgressCore::Lifecycle::Admission::Accepted) {
        operationId = g_lifecycle.operationId();
        g_manualComponent = manualSingleComponent
                                ? (firmwareSelected ? OtaProgressCore::Component::Firmware
                                                    : OtaProgressCore::Component::Filesystem)
                                : OtaProgressCore::Component::None;
        g_manualTotal = manualTotal;
        g_awaitingUploadSince = 0U;
        g_manualUploadTimerArmed = false;
        g_filesystemUnmounted = false;
    }
    portEXIT_CRITICAL(&g_otaLock);
    if (admission == OtaProgressCore::Lifecycle::Admission::Busy) return Admission::Busy;
    IndicatorService::instance().setOtaActive(true);
    MBXServerHandlers::requestOtaQuiescence();
    return Admission::Accepted;
}

auto OtaCoordinator::reserveSigned(const char *targetVersion, const bool firmwareSelected,
                                   const bool filesystemSelected, uint32_t &operationId) -> Admission {
    return reserve(targetVersion, firmwareSelected, filesystemSelected, false, 0U, operationId);
}

auto OtaCoordinator::reserveManual(const OtaProgressCore::Component component,
                                   const uint32_t totalBytes, uint32_t &operationId) -> Admission {
    if (component == OtaProgressCore::Component::None || totalBytes == 0U) {
        operationId = 0U;
        return Admission::Unavailable;
    }
    return reserve("manual", component == OtaProgressCore::Component::Firmware,
                   component == OtaProgressCore::Component::Filesystem, true,
                   totalBytes, operationId);
}

bool OtaCoordinator::quiesce(const uint32_t operationId, const bool awaitManualUpload) {
    if (!operationMatches(operationId)) return false;
    const MqttAdmissionResult mqttAdmission = g_mqtt->requestQuiesceForOta(operationId);
    const ModbusOperationResult modbusAdmission = g_modbus->requestQuiesceForOta(operationId);
    if (mqttAdmission != MqttAdmissionResult::Accepted
        || modbusAdmission != ModbusOperationResult::Success) {
        fail(operationId, "owner_unavailable");
        return false;
    }

    const uint32_t startedAt = millis();
    bool mqttReady = false;
    bool modbusReady = false;
    while (operationMatches(operationId)) {
        (void)ownersAcknowledged(operationId, mqttReady, modbusReady);
        const bool httpReady = MBXServerHandlers::otaQuiescenceReady();
        const auto wait = OtaQuiescenceCore::evaluate(
            startedAt, millis(), mqttReady, modbusReady, httpReady,
            OTA_MQTT_QUIESCE_TIMEOUT_MS, OTA_MODBUS_QUIESCE_TIMEOUT_MS,
            OTA_RUNTIME_DRAIN_TIMEOUT_MS);
        if (wait == OtaQuiescenceCore::Result::Ready) break;
        if (wait != OtaQuiescenceCore::Result::Pending) {
            fail(operationId, OtaQuiescenceCore::errorCode(wait));
            return false;
        }
        delay(10);
    }

    const uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t minimumHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const auto mqtt = g_mqtt->getStatusSnapshot();
    const auto modbus = g_modbus->getRuntimeSnapshot();
    char diagnostics[320];
    snprintf(diagnostics, sizeof(diagnostics),
             "OTA quiesced: heap=%lu min=%lu largest=%lu mqttQ=%u modbusQ=%u runtime=%u leases=%u normalSse=%u normalSseQ=%u otaSse=%u otaSseQ=%u mqttSignal=%s",
             static_cast<unsigned long>(freeHeap), static_cast<unsigned long>(minimumHeap),
             static_cast<unsigned long>(largestBlock), static_cast<unsigned>(mqtt.commandQueueDepth),
             static_cast<unsigned>(modbus.commandQueueDepth),
             static_cast<unsigned>(MBXServerHandlers::runtimeJobCount()),
             static_cast<unsigned>(MBXServerHandlers::uiFilesystemLeaseCount()),
             static_cast<unsigned>(MBXServerHandlers::normalEventClientCount()),
             static_cast<unsigned>(MBXServerHandlers::normalEventPacketsWaiting()),
             static_cast<unsigned>(MBXServerHandlers::otaEventClientCount()),
             static_cast<unsigned>(MBXServerHandlers::otaEventPacketsWaiting()),
             mqttOtaQuiesceResultToString(mqtt.otaQuiesceResult));
    if (g_logger != nullptr) g_logger->logInformation(diagnostics);

    if (!OtaProgressCore::meetsHeapFloor(
            freeHeap, largestBlock, OTA_MIN_INTERNAL_FREE_HEAP,
            OTA_MIN_INTERNAL_LARGEST_BLOCK)) {
        fail(operationId, "insufficient_heap");
        return false;
    }

    if (awaitManualUpload) {
        portENTER_CRITICAL(&g_otaLock);
        const bool marked = g_lifecycle.markAwaitingUpload(operationId);
        if (marked) {
            g_awaitingUploadSince = millis();
            g_manualUploadTimerArmed = true;
        }
        portEXIT_CRITICAL(&g_otaLock);
        if (!marked) {
            fail(operationId, "invalid_transition");
            return false;
        }
    }
    return true;
}

bool OtaCoordinator::validateManualUpload(const uint32_t operationId,
                                          const OtaProgressCore::Component component,
                                          uint32_t &declaredSize) {
    portENTER_CRITICAL(&g_otaLock);
    const auto current = g_lifecycle.snapshot();
    const bool valid = g_lifecycle.active() && current.operationId == operationId
                    && current.state == OtaProgressCore::State::AwaitingUpload
                    && component == g_manualComponent && g_manualTotal > 0U;
    declaredSize = valid ? g_manualTotal : 0U;
    if (valid) {
        g_awaitingUploadSince = millis();
        g_manualUploadTimerArmed = true;
    }
    portEXIT_CRITICAL(&g_otaLock);
    return valid;
}

bool OtaCoordinator::beginComponent(const uint32_t operationId,
                                    const OtaProgressCore::Component component,
                                    const uint32_t totalBytes) {
    if (!operationMatches(operationId)) return false;
    if (component == OtaProgressCore::Component::Filesystem && !g_filesystemUnmounted) {
        if (!MBXServerHandlers::unmountUiFilesystem()) return false;
        g_filesystemUnmounted = true;
    }
    // quiesce() checked the quiescence floors before any TLS session or
    // multipart request existed. Signed downloads then open TLS (roughly
    // 45 KiB of internal heap on ESP32) and a manual multipart request also
    // consumes memory after preflight, so recheck at the actual flash boundary.
    // That memory is already committed, so this stage uses the lower
    // flash-boundary floors instead of repeating the quiescence floors.
    const uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const bool admitted = OtaProgressCore::meetsHeapFloor(
        freeHeap, largestBlock, OTA_FLASH_MIN_INTERNAL_FREE_HEAP,
        OTA_FLASH_MIN_INTERNAL_LARGEST_BLOCK);
    char diagnostics[160];
    snprintf(diagnostics, sizeof(diagnostics),
             "OTA component %s: heap=%lu largest=%lu floor=%lu/%lu",
             admitted ? "admitted" : "refused",
             static_cast<unsigned long>(freeHeap),
             static_cast<unsigned long>(largestBlock),
             static_cast<unsigned long>(OTA_FLASH_MIN_INTERNAL_FREE_HEAP),
             static_cast<unsigned long>(OTA_FLASH_MIN_INTERNAL_LARGEST_BLOCK));
    if (!admitted) {
        logError(diagnostics);
        return false;
    }
    portENTER_CRITICAL(&g_otaLock);
    const bool result = g_lifecycle.beginComponent(operationId, component, totalBytes);
    portEXIT_CRITICAL(&g_otaLock);
    // Record the post-TLS reading on every admitted component for future tuning.
    if (result) logInformation(diagnostics);
    return result;
}

bool OtaCoordinator::updateProgress(const uint32_t operationId, const uint32_t receivedBytes) {
    portENTER_CRITICAL(&g_otaLock);
    const bool result = g_lifecycle.updateBytes(operationId, receivedBytes);
    if (result && g_manualComponent != OtaProgressCore::Component::None) {
        g_awaitingUploadSince = millis();
        g_manualUploadTimerArmed = true;
    }
    portEXIT_CRITICAL(&g_otaLock);
    return result;
}

bool OtaCoordinator::noteManualUploadActivity(const uint32_t operationId) {
    portENTER_CRITICAL(&g_otaLock);
    const auto current = g_lifecycle.snapshot();
    const bool accepted = g_lifecycle.active() && current.operationId == operationId
                       && g_manualComponent != OtaProgressCore::Component::None
                       && (current.state == OtaProgressCore::State::DownloadingWritingFirmware
                           || current.state == OtaProgressCore::State::DownloadingWritingFilesystem);
    if (accepted) {
        g_awaitingUploadSince = millis();
        g_manualUploadTimerArmed = true;
    }
    portEXIT_CRITICAL(&g_otaLock);
    return accepted;
}

bool OtaCoordinator::beginVerification(const uint32_t operationId,
                                       const OtaProgressCore::Component component) {
    portENTER_CRITICAL(&g_otaLock);
    const bool result = g_lifecycle.beginVerification(operationId, component);
    if (result && component == g_manualComponent) g_manualUploadTimerArmed = false;
    portEXIT_CRITICAL(&g_otaLock);
    return result;
}

bool OtaCoordinator::completeComponent(const uint32_t operationId,
                                       const OtaProgressCore::Component component) {
    portENTER_CRITICAL(&g_otaLock);
    const bool result = g_lifecycle.completeComponent(operationId, component);
    portEXIT_CRITICAL(&g_otaLock);
    return result;
}

bool OtaCoordinator::succeed(const uint32_t operationId) {
    portENTER_CRITICAL(&g_otaLock);
    const bool marked = g_lifecycle.markRebooting(operationId);
    portEXIT_CRITICAL(&g_otaLock);
    if (!marked) return false;
    revokeSessions();
    if (RebootScheduler::schedule() != RebootScheduler::ScheduleResult::Accepted) {
        fail(operationId, "reboot_unavailable");
        return false;
    }
    return true;
}

void OtaCoordinator::fail(const uint32_t operationId, const char *errorCode) {
    if (!operationMatches(operationId)) return;
    Update.abort();
    portENTER_CRITICAL(&g_otaLock);
    (void)g_lifecycle.failureDetected(operationId, errorCode);
    (void)g_lifecycle.markResuming(operationId);
    portEXIT_CRITICAL(&g_otaLock);

    bool uiMounted = !g_filesystemUnmounted;
    if (g_filesystemUnmounted) {
        uiMounted = SPIFFS.begin(false, "/spiffs", 10, "spiffs");
        g_filesystemUnmounted = false;
        if (!uiMounted) logError("OTA recovery: UI SPIFFS remount failed; compiled recovery remains active");
    }

    const auto mqttBefore = g_mqtt->getStatusSnapshot();
    const auto modbusBefore = g_modbus->getRuntimeSnapshot();
    if (mqttBefore.otaQuiesced && mqttBefore.otaOperationId == operationId) {
        (void)g_mqtt->requestResumeFromOta(operationId);
    } else {
        g_mqtt->cancelOtaQuiesceRequest(operationId);
    }
    if (modbusBefore.otaQuiesced && modbusBefore.otaOperationId == operationId) {
        (void)g_modbus->requestResumeFromOta(operationId);
    } else {
        g_modbus->cancelOtaQuiesceRequest(operationId);
    }

    const uint32_t resumeStartedAt = millis();
    while (millis() - resumeStartedAt < OTA_RESUME_TIMEOUT_MS) {
        const auto mqtt = g_mqtt->getStatusSnapshot();
        const auto modbus = g_modbus->getRuntimeSnapshot();
        if (!mqtt.otaQuiesced && !modbus.otaQuiesced
            && !g_mqtt->otaControlActive() && !g_modbus->otaControlActive()) break;
        delay(10);
    }
    MBXServerHandlers::resumeAfterOta(uiMounted);
    IndicatorService::instance().setOtaActive(false);
    revokeSessions();
    portENTER_CRITICAL(&g_otaLock);
    (void)g_lifecycle.markFailed(operationId, errorCode);
    g_manualComponent = OtaProgressCore::Component::None;
    g_manualTotal = 0U;
    g_awaitingUploadSince = 0U;
    g_manualUploadTimerArmed = false;
    portEXIT_CRITICAL(&g_otaLock);
}

bool OtaCoordinator::failAsync(const uint32_t operationId, const char *errorCode) {
    bool expected = false;
    if (!g_recoveryTaskActive.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    g_failureContext.operationId = operationId;
    OtaProgressCore::copyFixed(
        g_failureContext.errorCode, sizeof(g_failureContext.errorCode), errorCode);
    if (xTaskCreatePinnedToCore([](void *parameter) {
            auto *failure = static_cast<FailureContext *>(parameter);
            OtaCoordinator::fail(failure->operationId, failure->errorCode);
            g_recoveryTaskActive.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
        }, "otaRecovery", 6144, &g_failureContext, 1, nullptr, APP_CPU_NUM) != pdPASS) {
        // Keep recovery allocation-free if a task stack cannot be created. The
        // main owner loop will execute the same unwind on its next iteration.
        g_recoveryPending.store(true, std::memory_order_release);
    }
    return true;
}

OtaProgressCore::Snapshot OtaCoordinator::snapshot() {
    portENTER_CRITICAL(&g_otaLock);
    const OtaProgressCore::Snapshot result = g_lifecycle.snapshot();
    portEXIT_CRITICAL(&g_otaLock);
    return result;
}

bool OtaCoordinator::installationActive() {
    portENTER_CRITICAL(&g_otaLock);
    const bool result = g_lifecycle.active();
    portEXIT_CRITICAL(&g_otaLock);
    return result;
}

bool OtaCoordinator::ordinaryHttpAllowed() {
    return !installationActive();
}

bool OtaCoordinator::uiFilesystemAllowed() {
    return !installationActive() && !g_filesystemUnmounted;
}

bool OtaCoordinator::createSession(char tokenHex[65]) {
    if (tokenHex == nullptr || installationActive()) return false;
    uint8_t token[OtaSessionCore::TOKEN_BYTES];
    esp_fill_random(token, sizeof(token));
    portENTER_CRITICAL(&g_otaLock);
    const bool stored = !g_lifecycle.active() && g_sessions.create(token, millis());
    portEXIT_CRITICAL(&g_otaLock);
    if (stored) OtaSessionCore::encodeHexToken(token, tokenHex);
    volatile uint8_t *clear = token;
    for (size_t i = 0U; i < sizeof(token); ++i) clear[i] = 0U;
    return stored;
}

bool OtaCoordinator::authorizeSession(const char *tokenHex, const size_t length) {
    uint8_t token[OtaSessionCore::TOKEN_BYTES];
    if (!OtaSessionCore::decodeHexToken(tokenHex, length, token)) return false;
    portENTER_CRITICAL(&g_otaLock);
    const bool authorized = g_sessions.authorize(token, millis());
    portEXIT_CRITICAL(&g_otaLock);
    volatile uint8_t *clear = token;
    for (size_t i = 0U; i < sizeof(token); ++i) clear[i] = 0U;
    return authorized;
}

void OtaCoordinator::revokeSessions() {
    portENTER_CRITICAL(&g_otaLock);
    g_sessions.revokeAll();
    portEXIT_CRITICAL(&g_otaLock);
}

void OtaCoordinator::loop() {
    if (g_recoveryPending.exchange(false, std::memory_order_acq_rel)) {
        const FailureContext failure = g_failureContext;
        fail(failure.operationId, failure.errorCode);
        g_recoveryTaskActive.store(false, std::memory_order_release);
    }
    const uint32_t now = millis();
    portENTER_CRITICAL(&g_otaLock);
    g_sessions.expire(now);
    const auto current = g_lifecycle.snapshot();
    const bool uploadExpired = g_lifecycle.active()
                            && g_manualComponent != OtaProgressCore::Component::None
                            && OtaProgressCore::manualTransferTimedOut(
                                   current.state, g_manualUploadTimerArmed, now,
                                   g_awaitingUploadSince, OTA_MANUAL_UPLOAD_TIMEOUT_MS);
    const char *errorCode = current.state == OtaProgressCore::State::AwaitingUpload
                                ? "upload_start_timeout"
                                : "upload_idle_timeout";
    if (uploadExpired) (void)g_lifecycle.failureDetected(current.operationId, errorCode);
    portEXIT_CRITICAL(&g_otaLock);
    if (uploadExpired) {
        (void)failAsync(current.operationId, errorCode);
    }
}
