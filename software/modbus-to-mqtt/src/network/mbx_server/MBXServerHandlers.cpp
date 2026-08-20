#include "network/mbx_server/MBXServerHandlers.h"
#include <atomic>
#include <array>
#include <cstdlib>
#include <memory>
#include <new>
#include <functional>
#include "network/mbx_server/BodyAccumulator.h"
#include "mqtt/MqttConfigCore.h"
#include "mqtt/MqttConfigMutationCore.h"
#include "storage/ConfigFs.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <AsyncEventSource.h>
#include <Arduino.h>
#include "Config.h"

#include "constants/HttpMediaTypes.h"
#include "constants/HttpResponseCodes.h"
#include "constants/Routes.h"
#include "network/NetworkPortal.h"
#include "network/wifi/ProvisioningCompletionCore.h"
#include "network/wifi/WifiConfigurationStorage.h"
#include "network/mbx_server/MutationRequestGuardCore.h"
#include "services/StatService.h"
#include "services/FactoryResetCore.h"
#include "services/RebootScheduler.h"
#include "services/OtaService.h"
#include "services/ota/HttpOtaService.h"
#include "services/ota/OtaAuthorizationCore.h"
#include "services/ota/OtaCredentialCore.h"
#include "services/ota/OtaCredentialService.h"
#include "services/ota/OtaUploadGuardCore.h"
#include "services/IndicatorService.h"
#include "modbus/ModbusManager.h"
#include "modbus/ModbusConfigLoader.h"

auto constexpr OTA_FS_UPLOAD_BEGIN_FAIL_RESP = R"({"error":"ota_begin_failed"})";
auto constexpr OTA_FW_UPLOAD_BEGIN_FAIL_RESP = R"({"error":"ota_begin_failed"})";
auto constexpr OTA_END_FAIL_RESP = R"({"error":"ota_end_failed"})";
auto constexpr OTA_END_FW_UPLOAD_OK = R"({"ok":true,"type":"firmware"})";
auto constexpr OTA_END_FS_UPLOAD_OK = R"({"ok":true,"type":"filesystem"})";
auto constexpr BAD_REQUEST_RESP = R"({"error":"bad_request"})";
auto constexpr OTA_UNAUTHORIZED_RESP = R"({"error":"ota_password_required"})";
auto constexpr OTA_PASSWORD_INVALID_RESP = R"({"error":"invalid_password"})";
auto constexpr OTA_PASSWORD_STORAGE_RESP = R"({"error":"credential_storage_failed"})";
auto constexpr FACTORY_RESET_BUSY_RESP = R"({"error":"factory_reset_in_progress"})";
auto constexpr FACTORY_RESET_TASK_RESP = R"({"error":"factory_reset_task_failed"})";
auto constexpr FACTORY_RESET_CONFIG_RESP = R"({"error":"factory_reset_config_storage_failed"})";
auto constexpr FACTORY_RESET_NVS_RESP = R"({"error":"factory_reset_nvs_failed"})";
auto constexpr WIFI_HANDLER_OK_RESP = "{\"ok\":true}";
auto constexpr WIFI_ALREADY_CONNECTING_RESP = R"({"error":"already_connecting"})";
auto constexpr NETWORK_RESET_ACCEPTED_RESP = R"({"ok":true,"resetting":true})";
auto constexpr REBOOT_ACCEPTED_RESP = R"({"ok":true,"rebooting":true})";
auto constexpr REBOOT_PENDING_RESP = R"({"ok":false,"error":"reboot_pending"})";
auto constexpr REBOOT_UNAVAILABLE_RESP = R"({"ok":false,"error":"reboot_unavailable"})";
auto constexpr STATION_READY_RESP = R"({"ok":true,"mode":"station"})";

auto constexpr NETWORK_RESET_DELAY_MS = 5000;

std::atomic<NetworkPortal *> g_portal{nullptr};
static std::atomic<MemoryLogger *> g_memlog{nullptr};
static std::atomic<MqttManager *> g_comm{nullptr};
static std::atomic<ModbusManager *> g_mb{nullptr};
static AsyncEventSource g_events(Routes::EVENTS);
static const Logger *g_eventLogger = nullptr;
static std::atomic<bool> g_eventsAttached{false};
static std::atomic<uint32_t> g_lastPingAt{0};
static std::atomic<size_t> g_lastLogCursor{0};
static std::atomic<uint32_t> g_lastLogCheckAt{0};
static std::atomic<uint32_t> g_eventSeq{0};
static std::atomic<bool> g_otaHttpApplying{false};
static std::atomic<bool> g_factoryResetInProgress{false};
static QueueHandle_t g_runtimeJobQueue = nullptr;
static QueueHandle_t g_runtimeResponseQueue = nullptr;
static SemaphoreHandle_t g_runtimeJobSlots = nullptr;
static SemaphoreHandle_t g_configScheduleMutex = nullptr;
static SemaphoreHandle_t g_configStorageMutex = nullptr;
static std::array<TaskHandle_t, RUNTIME_HTTP_WORKER_COUNT> g_runtimeWorkerTasks{};
static std::atomic<size_t> g_runtimeWorkerCount{0U};

constexpr uint32_t STATS_PUSH_INTERVAL_MS = 5000;
constexpr uint32_t STATS_HEARTBEAT_MS = 30000;
constexpr uint32_t STATS_UPTIME_QUANTUM_MS = 10000;
constexpr uint32_t STATS_HEAP_QUANTUM_BYTES = 1024;
constexpr uint32_t LOGS_CHECK_INTERVAL_MS = 1200;
constexpr uint32_t EVENTS_PING_INTERVAL_MS = 30000;
constexpr uint32_t EVENT_RETRY_MS = 5000;
constexpr size_t LOG_CHUNK_BYTES = 2048;

namespace {

ProvisioningCompletionCore::ScheduleResult scheduleProvisioningReboot(void *) {
    switch (RebootScheduler::schedule()) {
        case RebootScheduler::ScheduleResult::Accepted:
            return ProvisioningCompletionCore::ScheduleResult::Accepted;
        case RebootScheduler::ScheduleResult::Pending:
            return ProvisioningCompletionCore::ScheduleResult::Pending;
        case RebootScheduler::ScheduleResult::Unavailable:
            return ProvisioningCompletionCore::ScheduleResult::Unavailable;
    }
    return ProvisioningCompletionCore::ScheduleResult::Unavailable;
}

constexpr uint32_t OTA_UPLOAD_STATE_MAGIC = 0x4F544155U;
constexpr const char *OTA_UPLOAD_STATE_ATTRIBUTE = "otaUploadState";

struct OtaUploadState {
    uint32_t magic;
    OtaUploadGuardCore::State guard;
};

struct OtaOperationContext {
    OtaOperationContext(const Logger *loggerValue, const bool firmwareValue)
        : logger(loggerValue), firmware(firmwareValue) {}

    const Logger *logger;
    bool firmware;
};

struct FactoryResetContext {
    AsyncWebServerRequestPtr request;
    const Logger *logger = nullptr;
    esp_err_t nvsResult = ESP_OK;
    esp_err_t nvsRecoveryResult = ESP_OK;
};

struct RuntimeResponse {
    int status = HttpResponseCodes::INTERNAL_SERVER_ERROR;
    String contentType = HttpMediaTypes::JSON;
    String body = R"({"error":"runtime_job_incomplete"})";
};

struct RuntimeJob {
    AsyncWebServerRequestPtr request;
    std::function<void(RuntimeResponse &)> operation;
    RuntimeResponse response;
};

void setRuntimeResponse(RuntimeResponse &response, const int status,
                        const char *contentType = HttpMediaTypes::JSON,
                        const String &body = {}) {
    response.status = status;
    response.contentType = contentType;
    response.body = body;
}

void setRuntimeJsonResponse(RuntimeResponse &response, const JsonDocument &document,
                            const int status = HttpResponseCodes::OK) {
    response.status = status;
    response.contentType = HttpMediaTypes::JSON;
    response.body.clear();
    serializeJson(document, response.body);
}

[[noreturn]] void runtimeWorker(void *) {
    while (true) {
        RuntimeJob *job = nullptr;
        if (xQueueReceive(g_runtimeJobQueue, &job, portMAX_DELAY) != pdTRUE || job == nullptr) continue;
        job->operation(job->response);
        // The completion queue is sized for every queued plus in-flight job,
        // so this non-blocking transfer is bounded and should always succeed.
        // Only the Arduino loop resumes paused HTTP requests; ESPAsyncWebServer
        // response state is therefore never advanced concurrently by workers.
        if (g_runtimeResponseQueue == nullptr
            || xQueueSend(g_runtimeResponseQueue, &job, 0) != pdTRUE) {
            delete job;
            if (g_runtimeJobSlots != nullptr) xSemaphoreGive(g_runtimeJobSlots);
        }
    }
}

bool scheduleRuntimeJob(AsyncWebServerRequest *request,
                        std::function<void(RuntimeResponse &)> operation) {
    if (request == nullptr || g_runtimeJobQueue == nullptr || g_runtimeResponseQueue == nullptr
        || g_runtimeJobSlots == nullptr
        || g_runtimeWorkerCount.load(std::memory_order_acquire) == 0U) return false;
    if (xSemaphoreTake(g_runtimeJobSlots, 0) != pdTRUE) return false;
    auto *job = new (std::nothrow) RuntimeJob;
    if (job == nullptr) {
        xSemaphoreGive(g_runtimeJobSlots);
        return false;
    }
    job->request = request->pause();
    job->operation = std::move(operation);
    if (xQueueSend(g_runtimeJobQueue, &job, 0) != pdTRUE) {
        delete job;
        xSemaphoreGive(g_runtimeJobSlots);
        return false;
    }
    return true;
}

template<typename Manager, typename Operation>
bool scheduleConfigurationJob(AsyncWebServerRequest *request,
                              Manager *manager,
                              Operation operation) {
    if (manager == nullptr || g_configScheduleMutex == nullptr) return false;
    if (xSemaphoreTake(g_configScheduleMutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    // Issuing a ticket is intentionally inert. The manager publishes it only
    // after the exact candidate reaches its owner queue, so a failed HTTP-job
    // admission cannot transiently supersede an older admitted candidate.
    const uint32_t revision = manager->issueConfigurationRevision();
    const std::function<void(RuntimeResponse &, uint32_t)> ownedOperation = std::move(operation);
    const bool scheduled = scheduleRuntimeJob(
        request,
        [ownedOperation, revision](RuntimeResponse &response) {
            ownedOperation(response, revision);
        });
    xSemaphoreGive(g_configScheduleMutex);
    return scheduled;
}

class ConfigurationStorageGuard {
public:
    ConfigurationStorageGuard()
        : _locked(g_configStorageMutex != nullptr
                  && xSemaphoreTake(g_configStorageMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {}

    ~ConfigurationStorageGuard() {
        if (_locked) xSemaphoreGive(g_configStorageMutex);
    }

    explicit operator bool() const { return _locked; }

private:
    bool _locked;
};

bool formatFactoryResetConfigStorage(void *) {
    return ConfigFS.format();
}

bool eraseFactoryResetNvsStorage(void *context) {
    auto *reset = static_cast<FactoryResetContext *>(context);
    WiFi.persistent(false);
    reset->nvsResult = nvs_flash_erase();
    if (reset->nvsResult != ESP_OK) reset->nvsRecoveryResult = nvs_flash_init();
    return reset->nvsResult == ESP_OK;
}

bool scheduleFactoryResetTask(void *context) {
    return xTaskCreatePinnedToCore([](void *parameter) {
        auto *reset = static_cast<FactoryResetContext *>(parameter);
        const FactoryResetCore::Operations operations{
            formatFactoryResetConfigStorage,
            eraseFactoryResetNvsStorage,
        };
        const FactoryResetCore::RunResult result = FactoryResetCore::execute(operations, reset);
        auto request = reset->request.lock();

        switch (result) {
            case FactoryResetCore::RunResult::Ok:
                if (request) {
                    request->send(HttpResponseCodes::ACCEPTED, HttpMediaTypes::JSON,
                                  R"({"ok":true,"rebooting":true})");
                }
                delete reset;
                delay(1000);
                ESP.restart();
                break;
            case FactoryResetCore::RunResult::ConfigStorageFailed:
                if (reset->logger) {
                    reset->logger->logError("Factory reset failed: config filesystem format failed");
                }
                if (request) {
                    request->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON,
                                  FACTORY_RESET_CONFIG_RESP);
                }
                delete reset;
                g_factoryResetInProgress.store(false, std::memory_order_release);
                break;
            case FactoryResetCore::RunResult::NvsStorageFailed:
                if (reset->logger) {
                    const String message = String("Factory reset failed: NVS erase returned ")
                        + static_cast<int>(reset->nvsResult) + "; recovery returned "
                        + static_cast<int>(reset->nvsRecoveryResult);
                    reset->logger->logError(message.c_str());
                }
                if (request) {
                    request->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON,
                                  FACTORY_RESET_NVS_RESP);
                }
                delete reset;
                g_factoryResetInProgress.store(false, std::memory_order_release);
                break;
        }
        request.reset();
        vTaskDelete(nullptr);
    }, "factoryReset", 4096, context, 1, nullptr, APP_CPU_NUM) == pdPASS;
}

bool beginOtaOperation(void *context) {
    const auto *operation = static_cast<OtaOperationContext *>(context);
    return operation->firmware
               ? OtaService::beginFirmware(0, operation->logger)
               : OtaService::beginFilesystem(0, operation->logger);
}

bool writeOtaOperation(void *context, uint8_t *data, const size_t len) {
    const auto *operation = static_cast<OtaOperationContext *>(context);
    return OtaService::write(data, len, operation->logger);
}

bool authorizeOtaRequest(void *context) {
    return MBXServerHandlers::isOtaRequestAuthorized(static_cast<AsyncWebServerRequest *>(context));
}

bool verifyOtaPassword(const uint8_t *password, const size_t passwordLength, void *) {
    return OtaCredentialService::verify(String(reinterpret_cast<const char *>(password), passwordLength));
}

OtaUploadState *otaUploadState(AsyncWebServerRequest *req) {
    if (req == nullptr || !req->getAttribute(OTA_UPLOAD_STATE_ATTRIBUTE, false)) return nullptr;
    auto *state = static_cast<OtaUploadState *>(req->_tempObject);
    return state != nullptr && state->magic == OTA_UPLOAD_STATE_MAGIC ? state : nullptr;
}

OtaUploadState *beginOtaUploadRequest(AsyncWebServerRequest *req) {
    OtaUploadState *state = otaUploadState(req);
    if (state == nullptr || !state->guard.authorizationChecked || !state->guard.authorized) {
        MBXServerHandlers::sendOtaUnauthorized(req);
        return nullptr;
    }
    return state;
}

}  // namespace

bool MBXServerHandlers::beginRuntimeWorker() {
    if (g_runtimeJobQueue != nullptr && g_runtimeResponseQueue != nullptr && g_runtimeJobSlots != nullptr
        && g_configScheduleMutex != nullptr && g_configStorageMutex != nullptr
        && g_runtimeWorkerCount.load(std::memory_order_acquire) > 0U) return true;
    g_runtimeJobQueue = xQueueCreate(RUNTIME_HTTP_JOB_QUEUE_DEPTH, sizeof(RuntimeJob *));
    if (g_runtimeJobQueue == nullptr) return false;
    g_runtimeResponseQueue = xQueueCreate(RUNTIME_HTTP_RESPONSE_QUEUE_DEPTH, sizeof(RuntimeJob *));
    if (g_runtimeResponseQueue == nullptr) {
        vQueueDelete(g_runtimeJobQueue);
        g_runtimeJobQueue = nullptr;
        return false;
    }
    g_runtimeJobSlots = xSemaphoreCreateCounting(RUNTIME_HTTP_RESPONSE_QUEUE_DEPTH,
                                                 RUNTIME_HTTP_RESPONSE_QUEUE_DEPTH);
    if (g_runtimeJobSlots == nullptr) {
        vQueueDelete(g_runtimeJobQueue);
        vQueueDelete(g_runtimeResponseQueue);
        g_runtimeJobQueue = nullptr;
        g_runtimeResponseQueue = nullptr;
        return false;
    }
    g_configScheduleMutex = xSemaphoreCreateMutex();
    g_configStorageMutex = xSemaphoreCreateMutex();
    if (g_configScheduleMutex == nullptr || g_configStorageMutex == nullptr) {
        vQueueDelete(g_runtimeJobQueue);
        vQueueDelete(g_runtimeResponseQueue);
        vSemaphoreDelete(g_runtimeJobSlots);
        if (g_configScheduleMutex != nullptr) vSemaphoreDelete(g_configScheduleMutex);
        if (g_configStorageMutex != nullptr) vSemaphoreDelete(g_configStorageMutex);
        g_runtimeJobQueue = nullptr;
        g_runtimeResponseQueue = nullptr;
        g_runtimeJobSlots = nullptr;
        g_configScheduleMutex = nullptr;
        g_configStorageMutex = nullptr;
        return false;
    }
    for (size_t i = 0; i < RUNTIME_HTTP_WORKER_COUNT; ++i) {
        const String taskName = String("runtimeHttp") + String(i);
        if (xTaskCreatePinnedToCore(runtimeWorker, taskName.c_str(), 6144, nullptr, 1,
                                    &g_runtimeWorkerTasks[i], APP_CPU_NUM) != pdPASS) break;
        g_runtimeWorkerCount.fetch_add(1U, std::memory_order_release);
    }
    if (g_runtimeWorkerCount.load(std::memory_order_acquire) == 0U) {
        vQueueDelete(g_runtimeJobQueue);
        vQueueDelete(g_runtimeResponseQueue);
        vSemaphoreDelete(g_runtimeJobSlots);
        vSemaphoreDelete(g_configScheduleMutex);
        vSemaphoreDelete(g_configStorageMutex);
        g_runtimeJobQueue = nullptr;
        g_runtimeResponseQueue = nullptr;
        g_runtimeJobSlots = nullptr;
        g_configScheduleMutex = nullptr;
        g_configStorageMutex = nullptr;
        return false;
    }
    return g_runtimeWorkerCount.load(std::memory_order_acquire) == RUNTIME_HTTP_WORKER_COUNT;
}

void MBXServerHandlers::pumpRuntimeResponses() {
    if (g_runtimeResponseQueue == nullptr) return;
    RuntimeJob *job = nullptr;
    if (xQueueReceive(g_runtimeResponseQueue, &job, 0) != pdTRUE || job == nullptr) return;
    auto request = job->request.lock();
    if (request) {
        if (job->response.body.length()) {
            request->send(job->response.status, job->response.contentType, job->response.body);
        } else {
            request->send(job->response.status);
        }
    }
    delete job;
    if (g_runtimeJobSlots != nullptr) xSemaphoreGive(g_runtimeJobSlots);
}

enum class StatsCategory : uint8_t {
    System = 0,
    Network,
    MQTT,
    Modbus,
    Storage,
    Health,
    Count
};

constexpr const char *STAT_EVENT_NAMES[] = {
    "stats-system",
    "stats-network",
    "stats-mqtt",
    "stats-modbus",
    "stats-storage",
    "stats-health"
};

static_assert(static_cast<size_t>(StatsCategory::Count) == (sizeof(STAT_EVENT_NAMES) / sizeof(STAT_EVENT_NAMES[0])),
              "STAT_EVENT_NAMES size mismatch");

static std::array<String, static_cast<size_t>(StatsCategory::Count)> g_statsPayload;
static std::array<uint32_t, static_cast<size_t>(StatsCategory::Count)> g_statsLastSent{};

bool parseConnectPayload(uint8_t *data, size_t len, String &ssid, String &pass,
                         String &bssid, bool &save, WifiStaticConfig &st,
                         uint8_t &channel) {
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, data, len);
    if (err) return false;

    ssid = doc["ssid"] | "";
    pass = doc["password"] | "";
    bssid = doc["bssid"] | "";
    save = doc["save"] | true;
    channel = static_cast<uint8_t>(doc["channel"] | 0);

    if (doc["static"].is<JsonObject>()) {
        const auto s = doc["static"].as<JsonObject>();
        st.ip = s["ip"] | "";
        st.gateway = s["gateway"] | "";
        st.subnet = s["subnet"] | s["mask"] | "";
        st.dns1 = s["dns1"] | "";
        st.dns2 = s["dns2"] | "";
    } else {
        st = {};
    }
    return true;
}

const char *stateToStr(const WifiConnectionState s) {
    switch (s) {
        case WifiConnectionState::Idle: return "idle";
        case WifiConnectionState::Connecting: return "connecting";
        case WifiConnectionState::Connected: return "connected";
        case WifiConnectionState::Failed: return "failed";
        case WifiConnectionState::Disconnected: return "disconnected";
    }
    return "unknown";
}

void sendJson(AsyncWebServerRequest *req, const JsonDocument &doc, const int status = HttpResponseCodes::OK) {
    String out;
    serializeJson(doc, out);
    req->send(status, HttpMediaTypes::JSON, out);
}

namespace {

bool eventStreamReady() {
    return g_eventsAttached.load(std::memory_order_acquire);
}

bool eventStreamHasClients() {
    return eventStreamReady() && g_events.count() > 0;
}

uint32_t nextEventId() {
    return g_eventSeq.fetch_add(1, std::memory_order_relaxed) + 1;
}

size_t statsIndex(const StatsCategory c) {
    return static_cast<size_t>(c);
}

uint32_t roundDown(const uint32_t value, const uint32_t quantum) {
    if (quantum == 0) return value;
    return (value / quantum) * quantum;
}

String buildStatsPayload(const StatsCategory cat) {
    JsonDocument doc;
    switch (cat) {
        case StatsCategory::System: {
            doc = StatService::appendSystemStats(doc, g_eventLogger);
            const uint32_t uptime = doc["uptimeMs"] | 0;
            doc["uptimeMs"] = roundDown(uptime, STATS_UPTIME_QUANTUM_MS);
            doc["heapFree"] = roundDown(static_cast<uint32_t>(doc["heapFree"] | 0), STATS_HEAP_QUANTUM_BYTES);
            doc["heapMin"] = roundDown(static_cast<uint32_t>(doc["heapMin"] | 0), STATS_HEAP_QUANTUM_BYTES);
            break;
        }
        case StatsCategory::Network: {
            doc = StatService::appendNetworkStats(doc);
            break;
        }
        case StatsCategory::MQTT: {
            doc = StatService::appendMQTTStats(doc);
            break;
        }
        case StatsCategory::Modbus: {
            doc = StatService::appendModbusStats(doc);
            break;
        }
        case StatsCategory::Storage: {
            doc = StatService::appendStorageStats(doc);
            break;
        }
        case StatsCategory::Health: {
            doc = StatService::appendHealthStats(doc);
            break;
        }
        case StatsCategory::Count:
        default: break;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

void sendStatsPayload(const StatsCategory cat, const String &payload, const uint32_t now, AsyncEventSourceClient *client) {
    const auto idx = statsIndex(cat);
    g_statsPayload[idx] = payload;
    g_statsLastSent[idx] = now;
    if (client) {
        client->send(payload.c_str(), STAT_EVENT_NAMES[idx], nextEventId());
    } else {
        g_events.send(payload.c_str(), STAT_EVENT_NAMES[idx], nextEventId());
    }
}

void emitStats(bool force, AsyncEventSourceClient *client) {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < static_cast<uint8_t>(StatsCategory::Count); ++i) {
        const auto cat = static_cast<StatsCategory>(i);
        const String payload = buildStatsPayload(cat);
        if (payload.isEmpty()) continue;

        const auto idx = statsIndex(cat);
        const bool changed = payload != g_statsPayload[idx];
        const uint32_t last = g_statsLastSent[idx];
        const bool heartbeat = (now - last) >= STATS_HEARTBEAT_MS;

        if (force || changed || heartbeat) {
            sendStatsPayload(cat, payload, now, client);
        }
    }
}

String readLogChunk(MemoryLogger *mem, size_t start, size_t len) {
    String out;
    if (!mem || len == 0) {
        return out;
    }
    std::unique_ptr<char[]> buf(new char[len + 1]);
    const size_t wrote = mem->copyAsText(start, reinterpret_cast<uint8_t *>(buf.get()), len);
    buf[wrote] = '\0';
    out = buf.get();
    return out;
}

void sendLogPayload(const String &text, const bool truncated, const char *eventName) {
    if (text.isEmpty()) {
        return;
    }
    JsonDocument doc;
    doc["text"] = text;
    doc["truncated"] = truncated;
    String payload;
    serializeJson(doc, payload);
    g_events.send(payload.c_str(), eventName, nextEventId());
}

void sendOtaStatus(const char *stage,
                   const uint32_t received,
                   const uint32_t total,
                   const char *detail) {
    if (!stage || !eventStreamHasClients()) {
        return;
    }
    JsonDocument doc;
    doc["stage"] = stage;
    if (received || total) {
        doc["received"] = received;
        doc["total"] = total;
    }
    if (detail && detail[0] != '\0') {
        doc["detail"] = detail;
    }
    String payload;
    serializeJson(doc, payload);
    g_events.send(payload.c_str(), "ota-status", nextEventId());
}

void sendInitialLogsToClient(AsyncEventSourceClient *client) {
    auto *mem = g_memlog.load(std::memory_order_acquire);
    if (!client || !mem) {
        return;
    }

    const size_t total = mem->flattenedSize();
    if (total == 0) {
        return;
    }
    const size_t start = (total > LOG_CHUNK_BYTES) ? (total - LOG_CHUNK_BYTES) : 0;
    const String text = readLogChunk(mem, start, total - start);
    if (text.isEmpty()) {
        return;
    }

    JsonDocument doc;
    doc["text"] = text;
    doc["truncated"] = start > 0;
    String payload;
    serializeJson(doc, payload);
    client->send(payload.c_str(), "logs", nextEventId());
}

void broadcastLogDelta() {
    auto *mem = g_memlog.load(std::memory_order_acquire);
    if (!mem) {
        return;
    }

    const size_t total = mem->flattenedSize();
    if (total == 0) {
        g_lastLogCursor.store(0, std::memory_order_relaxed);
        return;
    }

    size_t lastCursor = g_lastLogCursor.load(std::memory_order_relaxed);
    if (lastCursor > total) {
        // Buffer rolled; send the latest window
        const size_t start = (total > LOG_CHUNK_BYTES) ? (total - LOG_CHUNK_BYTES) : 0;
        const String text = readLogChunk(mem, start, total - start);
        sendLogPayload(text, true, "logs");
        g_lastLogCursor.store(total, std::memory_order_relaxed);
        return;
    }

    size_t remaining = total - lastCursor;
    if (remaining == 0) {
        return;
    }

    size_t offset = lastCursor;
    while (remaining > 0) {
        const size_t chunk = remaining > LOG_CHUNK_BYTES ? LOG_CHUNK_BYTES : remaining;
        const String text = readLogChunk(mem, offset, chunk);
        sendLogPayload(text, false, "log");
        offset += chunk;
        remaining -= chunk;
    }
    g_lastLogCursor.store(total, std::memory_order_relaxed);
}

} // namespace

void MBXServerHandlers::setPortal(NetworkPortal *portal) {
    g_portal.store(portal, std::memory_order_release);
}

void MBXServerHandlers::setMemoryLogger(MemoryLogger *mem) {
    g_memlog.store(mem, std::memory_order_release);
}

void MBXServerHandlers::initEventStream(AsyncWebServer *server, const Logger *logger) {
    g_eventLogger = logger;
    bool expected = false;
    if (!g_eventsAttached.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    HttpOtaService::setProgressCallback(sendOtaStatus);

    g_events.onConnect([](AsyncEventSourceClient *client) {
        if (!client) {
            return;
        }
        client->send("ready", "ping", nextEventId(), EVENT_RETRY_MS);

        // Warm-up snapshot for new client
        emitStats(true, client);
        sendInitialLogsToClient(client);
    });

    server->addHandler(&g_events);
}

void MBXServerHandlers::pumpEventStream() {
    if (!eventStreamHasClients()) {
        return;
    }

    const uint32_t now = millis();
    static uint32_t lastStatsPoll = 0;
    if (now - lastStatsPoll >= STATS_PUSH_INTERVAL_MS) {
        lastStatsPoll = now;
        emitStats(false, nullptr);
    }

    if (now - g_lastLogCheckAt.load(std::memory_order_relaxed) >= LOGS_CHECK_INTERVAL_MS) {
        g_lastLogCheckAt.store(now, std::memory_order_relaxed);
        broadcastLogDelta();
    }

    if (now - g_lastPingAt.load(std::memory_order_relaxed) >= EVENTS_PING_INTERVAL_MS) {
        g_lastPingAt.store(now, std::memory_order_relaxed);
        g_events.send("ping", "ping", nextEventId());
    }
}

void MBXServerHandlers::handleCaptivePortalRedirect(AsyncWebServerRequest *req) {
    const IPAddress apIp = WiFi.softAPIP();
    const String target = String("http://") + apIp.toString() + Routes::ROOT;
    String html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
    html += R"(<meta http-equiv="refresh" content="0; url=)" + target + "\">";
    html += "<title>Configuration Portal</title></head><body>";
    html += "<p>Redirecting to <a href=\"" + target + "\">configuration portal</a>...</p>";
    html += "</body></html>";

    auto *response = req->beginResponse(HttpResponseCodes::OK, HttpMediaTypes::HTML, html);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Location", target);
    req->send(response);
}

void MBXServerHandlers::setMqttManager(MqttManager *mqttManager) {
    g_comm.store(mqttManager, std::memory_order_release);
}

MqttManager *MBXServerHandlers::getMqttManager() {
    return g_comm.load(std::memory_order_acquire);
}

void MBXServerHandlers::setModbusManager(ModbusManager *modbusManager) {
    g_mb.store(modbusManager, std::memory_order_release);
}

ModbusManager *MBXServerHandlers::getModbusManager() {
    return g_mb.load(std::memory_order_acquire);
}

void MBXServerHandlers::getSsidListAsJson(AsyncWebServerRequest *req) {
    auto *portal = g_portal.load(std::memory_order_acquire);
    if (!portal) {
        req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::JSON, "[]");
        return;
    }

    auto snap = portal->getLatestScanResultsSnapshot();

    String out;
    out.reserve(snap ? (snap->size() * 64 + 2) : 2);
    out += "[";

    if (snap && !snap->empty()) {
        for (size_t i = 0; i < snap->size(); ++i) {
            const auto &ap = (*snap)[i];

            String escSsid;
            escSsid.reserve(ap.SSID.length() + 4);
            for (char c: ap.SSID) {
                if (c == '\\' || c == '\"') {
                    escSsid += '\\';
                    escSsid += c;
                } else if (static_cast<uint8_t>(c) < 0x20) { escSsid += ' '; } else { escSsid += c; }
            }

            char bssidBuf[18] = {0};
            if (ap.hasBSSID) {
                snprintf(bssidBuf, sizeof(bssidBuf),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         ap.BSSID[0], ap.BSSID[1], ap.BSSID[2],
                         ap.BSSID[3], ap.BSSID[4], ap.BSSID[5]);
            }

            const bool isOpen = (ap.encryptionType == WIFI_AUTH_OPEN);
            const char *authName = isOpen
                                       ? "OPEN"
                                       : (ap.encryptionType == WIFI_AUTH_WEP
                                              ? "WEP"
                                              : (ap.encryptionType == WIFI_AUTH_WPA_PSK
                                                     ? "WPA"
                                                     : (ap.encryptionType == WIFI_AUTH_WPA2_PSK
                                                            ? "WPA2"
                                                            : (ap.encryptionType == WIFI_AUTH_WPA_WPA2_PSK
                                                                   ? "WPA/WPA2"
                                                                   : (ap.encryptionType == WIFI_AUTH_WPA2_ENTERPRISE
                                                                          ? "WPA2-ENT"
                                                                          : (ap.encryptionType == WIFI_AUTH_WPA3_PSK
                                                                                 ? "WPA3"
                                                                                 : "UNKNOWN"))))));

            out += R"({"ssid":")";
            out += escSsid;
            out += "\"";
            out += ",\"rssi\":";
            out += String(static_cast<long>(ap.RSSI));
            out += R"(,"secure":)";
            out += (ap.encryptionType == WIFI_AUTH_OPEN ? "false" : "true");
            out += R"(,"auth":")";
            out += authName;
            out += "\"";
            out += ",\"channel\":";
            out += String(static_cast<unsigned>(ap.channel));
            if (ap.hasBSSID) {
                out += R"(,"bssid":")";
                out += bssidBuf;
                out += "\"";
            }
            out += "}";

            if (i + 1 < snap->size()) out += ",";
        }
    }

    out += "]";
    req->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, out);
}

void MBXServerHandlers::handleNetworkReset(AsyncWebServerRequest *req) {
    if (req == nullptr) return;
    req->send(HttpResponseCodes::ACCEPTED, HttpMediaTypes::JSON, NETWORK_RESET_ACCEPTED_RESP);
    Serial.println("MBXServerHandlers::handleNetworkReset called");
    xTaskCreatePinnedToCore([](void *) {
        delay(500);
        const bool staticConfigurationCleared = WifiConfigurationStorage::clear();
        Serial.printf("handleNetworkReset: WifiConfigurationStorage::clear() -> %s\n",
                      staticConfigurationCleared ? "true" : "false");
        WiFi.persistent(true);
        WiFi.setAutoReconnect(false);
        esp_wifi_set_storage(WIFI_STORAGE_FLASH);

        wifi_config_t emptyStaConfig{};
        const esp_err_t cfgRes = esp_wifi_set_config(WIFI_IF_STA, &emptyStaConfig);
        Serial.printf("handleNetworkReset: esp_wifi_set_config(WIFI_IF_STA) -> %d\n", static_cast<int>(cfgRes));
        const esp_err_t restoreRes = esp_wifi_restore();
        Serial.printf("handleNetworkReset: esp_wifi_restore() -> %d\n", static_cast<int>(restoreRes));
        WiFi.disconnect(true, true);
        const bool eraseOk = WiFi.eraseAP(); // clear credentials stored in NVS
        WiFi.eraseAP();
        Serial.printf("handleNetworkReset: WiFi.eraseAP() -> %s\n", eraseOk ? "true" : "false");

        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        WiFi.persistent(false);
        WiFiClass::mode(WIFI_MODE_APSTA);
        vTaskDelay(pdMS_TO_TICKS(200)); // let flash writes finish
        ESP.restart();
    }, "netReset", 4096, nullptr, 1, nullptr, APP_CPU_NUM);
}

namespace {
// Per-request body handlers use ESPAsyncWebServer's request-scoped _tempObject
// instead of function-local statics, otherwise concurrent or aborted requests
// corrupt the next caller.
// _tempObject is freed with free(), so it can hold malloc/calloc buffers or
// simple POD flags, but not heap-allocated C++ objects with destructors.

// Routes a chunk-body handler error to the UI log terminal via MemoryLogger.
// No-op if the memory logger has not been wired yet (early boot only).
void logHandlerError(const char *msg) {
    if (auto *mem = g_memlog.load(std::memory_order_acquire)) {
        mem->logError(msg);
    }
}

bool writeConfigurationFile(const char *path, const char *value, const size_t length,
                            const char *errorPrefix) {
    File file = ConfigFS.open(path, FILE_WRITE);
    if (!file) {
        logHandlerError(errorPrefix);
        return false;
    }
    const size_t written = file.write(reinterpret_cast<const uint8_t *>(value), length);
    file.close();
    if (written != length) {
        logHandlerError("Configuration write was incomplete (config FS full?)");
        return false;
    }
    return true;
}

bool writeMqttConfig(const char *value, const size_t length, void *) {
    return writeConfigurationFile(ConfigFs::kMqttConfigFile, value, length,
                                  "PUT /api/config/mqtt: failed to open mqtt.json for writing");
}

bool acceptValidatedMqttConfig(const char *, size_t, void *) {
    return true;
}

bool writeModbusConfig(const char *value, const size_t length) {
    return writeConfigurationFile(ConfigFs::kModbusConfigFile, value, length,
                                  "PUT /api/config/modbus: failed to open config.json for writing");
}

void setSupersededResponse(RuntimeResponse &response, const char *configuration) {
    JsonDocument document;
    document["error"] = String(configuration) + "_configuration_superseded";
    document["reason"] = "superseded";
    setRuntimeJsonResponse(response, document, HttpResponseCodes::CONFLICT);
}

void setStorageBusyResponse(RuntimeResponse &response) {
    JsonDocument document;
    document["error"] = "configuration_storage_busy";
    setRuntimeJsonResponse(response, document, HttpResponseCodes::SERVICE_UNAVAILABLE);
}

bool writeMqttPassword(const char *value, const size_t length, void *) {
    const String password(value == nullptr ? "" : value);
    if (password.length() != length) {
        logHandlerError("POST /api/config/mqtt/secret: password contains unsupported NUL bytes");
        return false;
    }

    Preferences preferences;
    if (!preferences.begin(MQTT_PREFS_NAMESPACE, false)) {
        logHandlerError("POST /api/config/mqtt/secret: failed to open password storage");
        return false;
    }
    const size_t written = preferences.putString("pass", password);
    const bool stored = written == length
                        && preferences.isKey("pass")
                        && preferences.getString("pass") == password;
    preferences.end();
    if (!stored) {
        logHandlerError("POST /api/config/mqtt/secret: failed to store password");
        return false;
    }
    return true;
}

void sendMqttMutationResult(AsyncWebServerRequest *req, const MqttConfigMutationCore::Result &result) {
    using MqttConfigMutationCore::Status;
    if (result.status == Status::Ok) {
        req->send(HttpResponseCodes::NO_CONTENT);
        return;
    }

    JsonDocument document;
    int status = HttpResponseCodes::BAD_REQUEST_HTTP;
    switch (result.status) {
        case Status::InvalidJson:
            document["error"] = "invalid_json";
            break;
        case Status::InvalidField:
            document["error"] = "invalid_mqtt_field";
            document["field"] = MqttConfigCore::fieldName(result.validation.field);
            document["constraint"] = MqttConfigCore::constraintMessage(result.validation);
            if (result.validation.constraint == MqttConfigCore::Constraint::MaximumBytes) {
                document["maximumBytes"] = MqttConfigCore::maximumBytes(result.validation.field);
            } else if (result.validation.constraint == MqttConfigCore::Constraint::PortRange) {
                document["minimum"] = MqttConfigCore::PORT_MIN;
                document["maximum"] = MqttConfigCore::PORT_MAX;
            }
            break;
        case Status::StorageFailed:
            status = HttpResponseCodes::INTERNAL_SERVER_ERROR;
            document["error"] = "mqtt_storage_failed";
            break;
        case Status::ReloadFailed:
            status = HttpResponseCodes::INTERNAL_SERVER_ERROR;
            document["error"] = "mqtt_reload_failed";
            break;
        case Status::Ok:
            break;
    }

    String response;
    serializeJson(document, response);
    req->send(status, HttpMediaTypes::JSON, response);
}
}  // namespace

void MBXServerHandlers::handlePutModbusConfigBody(AsyncWebServerRequest *req, const uint8_t *data, const size_t len,
                                                  const size_t index,
                                                  const size_t total) {
    char *body = BodyAccumulator::append(req->_tempObject, data, len, index, total);
    if (body == nullptr) {
        if (index + len == total) {
            logHandlerError("PUT /api/config/modbus: out of memory accumulating request body");
            req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        }
        return;
    }

    const String configurationJson(body);
    ConfigurationRoot validatedCandidate;
    if (!ModbusConfigLoader::parseConfiguration(nullptr, configurationJson.c_str(),
                                                configurationJson.length(), validatedCandidate)) {
        req->send(HttpResponseCodes::BAD_REQUEST_HTTP, HttpMediaTypes::JSON,
                  R"({"error":"invalid_modbus_configuration"})");
        return;
    }
    auto *mb = g_mb.load(std::memory_order_acquire);
    if (mb == nullptr) {
        req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::JSON,
                  R"({"error":"modbus_unavailable"})");
        return;
    }
    if (!scheduleConfigurationJob(
            req, mb,
            [mb, configurationJson](RuntimeResponse &runtimeResponse, const uint32_t revision) {
                {
                    ConfigurationStorageGuard storage;
                    if (!storage) {
                        setStorageBusyResponse(runtimeResponse);
                        return;
                    }
                    if (mb->isConfigurationRevisionObsolete(revision)) {
                        setSupersededResponse(runtimeResponse, "modbus");
                        return;
                    }
                    if (!writeModbusConfig(configurationJson.c_str(), configurationJson.length())) {
                        JsonDocument response;
                        response["error"] = "modbus_storage_failed";
                        setRuntimeJsonResponse(runtimeResponse, response,
                                               HttpResponseCodes::INTERNAL_SERVER_ERROR);
                        return;
                    }
                }
                const ModbusOperationResult result = mb->requestReconfigure(configurationJson, revision);
                if (result == ModbusOperationResult::Success) {
                    setRuntimeResponse(runtimeResponse, HttpResponseCodes::NO_CONTENT);
                } else if (result == ModbusOperationResult::Superseded) {
                    setSupersededResponse(runtimeResponse, "modbus");
                } else {
                    JsonDocument response;
                    response["error"] = "modbus_reload_failed";
                    response["reason"] = modbusOperationResultToString(result);
                    setRuntimeJsonResponse(runtimeResponse, response, HttpResponseCodes::SERVICE_UNAVAILABLE);
                }
            })) {
        req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::JSON,
                  R"({"error":"runtime_queue_full"})");
    }
}

void MBXServerHandlers::handleModbusDisable(AsyncWebServerRequest *req, bool state) {
        if (auto *mb = g_mb.load(std::memory_order_acquire)) {
            if (!scheduleRuntimeJob(req, [mb, state](RuntimeResponse &runtimeResponse) {
                    const ModbusOperationResult result = mb->setEnabled(state);
                    if (result == ModbusOperationResult::Success) {
                        setRuntimeResponse(runtimeResponse, HttpResponseCodes::OK);
                    } else {
                        JsonDocument response;
                        response["error"] = "modbus_state_change_failed";
                        response["reason"] = modbusOperationResultToString(result);
                        setRuntimeJsonResponse(runtimeResponse, response, HttpResponseCodes::SERVICE_UNAVAILABLE);
                    }
                })) {
                req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::JSON,
                          R"({"error":"runtime_queue_full"})");
            }
            return;
        }
        req->send(HttpResponseCodes::OK);
}

void MBXServerHandlers::handlePutMqttConfigBody(AsyncWebServerRequest *req, const uint8_t *data, const size_t len,
                                                const size_t index, const size_t total) {
    char *body = BodyAccumulator::append(req->_tempObject, data, len, index, total);
    if (body == nullptr) {
        if (index + len == total) {
            logHandlerError("PUT /api/config/mqtt: out of memory accumulating request body");
            req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        }
        return;
    }

    const String configurationJson(body);
    const MqttConfigMutationCore::ConfigOperations validationOperations{
        acceptValidatedMqttConfig, nullptr};
    const MqttConfigMutationCore::Result validation = MqttConfigMutationCore::applyConfig(
        configurationJson.c_str(), configurationJson.length(), validationOperations, nullptr);
    if (validation.status != MqttConfigMutationCore::Status::Ok) {
        sendMqttMutationResult(req, validation);
        return;
    }
    auto *link = g_comm.load(std::memory_order_acquire);
    if (link == nullptr) {
        req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::JSON,
                  R"({"error":"mqtt_unavailable"})");
        return;
    }
    if (!scheduleConfigurationJob(
            req, link,
            [link, configurationJson](RuntimeResponse &runtimeResponse, const uint32_t revision) {
                MqttConfigMutationCore::Result mutation;
                {
                    ConfigurationStorageGuard storage;
                    if (!storage) {
                        setStorageBusyResponse(runtimeResponse);
                        return;
                    }
                    if (link->isConfigurationRevisionObsolete(revision)) {
                        setSupersededResponse(runtimeResponse, "mqtt");
                        return;
                    }
                    const MqttConfigMutationCore::ConfigOperations operations{writeMqttConfig, nullptr};
                    mutation = MqttConfigMutationCore::applyConfig(
                        configurationJson.c_str(), configurationJson.length(), operations, nullptr);
                }
                if (mutation.status != MqttConfigMutationCore::Status::Ok) {
                    JsonDocument response;
                    int status = HttpResponseCodes::BAD_REQUEST_HTTP;
                    if (mutation.status == MqttConfigMutationCore::Status::InvalidJson) {
                        response["error"] = "invalid_json";
                    } else if (mutation.status == MqttConfigMutationCore::Status::InvalidField) {
                        response["error"] = "invalid_mqtt_field";
                        response["field"] = MqttConfigCore::fieldName(mutation.validation.field);
                        response["constraint"] = MqttConfigCore::constraintMessage(mutation.validation);
                    } else {
                        response["error"] = "mqtt_storage_failed";
                        status = HttpResponseCodes::INTERNAL_SERVER_ERROR;
                    }
                    setRuntimeJsonResponse(runtimeResponse, response, status);
                    return;
                }

                const MqttOperationResult operation = link->requestReconfigure(configurationJson, revision);
                if (operation == MqttOperationResult::Success) {
                    setRuntimeResponse(runtimeResponse, HttpResponseCodes::NO_CONTENT);
                } else if (operation == MqttOperationResult::Superseded) {
                    setSupersededResponse(runtimeResponse, "mqtt");
                } else {
                    JsonDocument response;
                    response["error"] = "mqtt_reload_failed";
                    response["reason"] = mqttOperationResultToString(operation);
                    setRuntimeJsonResponse(runtimeResponse, response, HttpResponseCodes::SERVICE_UNAVAILABLE);
                }
            })) {
        req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::JSON,
                  R"({"error":"runtime_queue_full"})");
    }
}

void MBXServerHandlers::handlePutMqttSecretBody(AsyncWebServerRequest *req, const uint8_t *data, const size_t len,
                                                const size_t index, const size_t total) {
    char *body = BodyAccumulator::append(req->_tempObject, data, len, index, total);
    if (body == nullptr) {
        if (index + len == total) {
            logHandlerError("POST /api/config/mqtt/secret: out of memory accumulating request body");
            req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        }
        return;
    }

    const MqttConfigMutationCore::SecretOperations operations{writeMqttPassword};
    sendMqttMutationResult(req, MqttConfigMutationCore::applySecret(body, total, operations, nullptr));
}

void MBXServerHandlers::handleGetMqttConstraints(AsyncWebServerRequest *req) {
    JsonDocument document;
    document["brokerIpMaxBytes"] = MqttConfigCore::BROKER_MAX_BYTES;
    document["brokerUrlHostMaxBytes"] = MqttConfigCore::BROKER_MAX_BYTES;
    document["brokerMaxBytes"] = MqttConfigCore::BROKER_MAX_BYTES;
    document["portMin"] = MqttConfigCore::PORT_MIN;
    document["portMax"] = MqttConfigCore::PORT_MAX;
    document["portMaxBytes"] = MqttConfigCore::PORT_MAX_BYTES;
    document["userMaxBytes"] = MqttConfigCore::USER_MAX_BYTES;
    document["passwordMaxBytes"] = MqttConfigCore::PASSWORD_MAX_BYTES;
    sendJson(req, document);
}

void MBXServerHandlers::handleWifiConnect(AsyncWebServerRequest *req, WifiConnectionController &wifi,
                                          const uint8_t *data, const size_t len,
                                          const size_t index, const size_t total) {
    char *body = BodyAccumulator::append(req->_tempObject, data, len, index, total);
    if (body == nullptr) {
        if (index + len == total) {
            logHandlerError("POST /api/wifi/connect: out of memory accumulating request body");
            req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        }
        return;
    }

    String ssid, pass, bssid;
    uint8_t channel = 0;
    bool save = true;
    WifiStaticConfig st;

    if (!parseConnectPayload(reinterpret_cast<uint8_t *>(body), total, ssid, pass, bssid, save, st, channel)
        || ssid.isEmpty()) {
        req->send(HttpResponseCodes::BAD_REQUEST, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        return;
    }

    const bool accepted = wifi.connect(ssid, pass, bssid, st, save, channel);
    if (!accepted) {
        req->send(HttpResponseCodes::CONFLICT, HttpMediaTypes::JSON, WIFI_ALREADY_CONNECTING_RESP);
        return;
    }
    if (auto *p = g_portal.load(std::memory_order_acquire)) {
        p->suspendScanning(true);
    }

    req->send(HttpResponseCodes::ACCEPTED, HttpMediaTypes::JSON, WIFI_HANDLER_OK_RESP);
}

void MBXServerHandlers::handleWifiStatus(AsyncWebServerRequest *req, const WifiConnectionController &wifi) {
    const WifiStatus s = wifi.getStatus();
    if (s.state == WifiConnectionState::Connected
        || s.state == WifiConnectionState::Failed
        || s.state == WifiConnectionState::Disconnected) {
        if (auto *p = g_portal.load(std::memory_order_acquire)) {
            p->suspendScanning(false);
        }
    }
    JsonDocument doc;
    doc["state"] = stateToStr(s.state);
    doc["ssid"] = s.ssid;
    if (s.hasIp) doc["ip"] = s.ip;
    if (s.reason.length()) doc["reason"] = s.reason;
    doc["provisioningReady"] = s.provisioningReady;

    sendJson(req, doc);
}

void MBXServerHandlers::handleWifiCancel(AsyncWebServerRequest *req, WifiConnectionController &wifi) {
    wifi.cancel();
    req->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, WIFI_HANDLER_OK_RESP);
}

void MBXServerHandlers::handleWifiApOff(AsyncWebServerRequest *req, const WifiConnectionController &wifi,
                                       const Logger *logger) {
    if (req == nullptr) return;
    const WifiStatus status = wifi.getStatus();
    const auto result = ProvisioningCompletionCore::complete(
        status.state == WifiConnectionState::Connected, status.ip, status.provisioningReady,
        scheduleProvisioningReboot, nullptr);
    switch (result) {
        case ProvisioningCompletionCore::Result::NotReady:
            req->send(HttpResponseCodes::CONFLICT, HttpMediaTypes::JSON,
                      ProvisioningCompletionCore::NOT_READY_BODY);
            return;
        case ProvisioningCompletionCore::Result::Accepted: {
            if (logger) logger->logInformation("Provisioning complete; device reboot scheduled");
            req->send(HttpResponseCodes::ACCEPTED, HttpMediaTypes::JSON,
                      ProvisioningCompletionCore::acceptedBody(status.ip));
            return;
        }
        case ProvisioningCompletionCore::Result::Pending:
            req->send(HttpResponseCodes::CONFLICT, HttpMediaTypes::JSON,
                      ProvisioningCompletionCore::PENDING_BODY);
            return;
        case ProvisioningCompletionCore::Result::Unavailable:
            if (logger) logger->logError("Provisioning reboot scheduling failed: task unavailable");
            req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::JSON,
                      ProvisioningCompletionCore::UNAVAILABLE_BODY);
            return;
    }
}

void MBXServerHandlers::handleSystemReady(AsyncWebServerRequest *req) {
    if (req == nullptr) return;
    auto *response = req->beginResponse(HttpResponseCodes::OK, HttpMediaTypes::JSON, STATION_READY_RESP);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Access-Control-Allow-Origin", "*");
    req->send(response);
}

void MBXServerHandlers::getSystemStats(AsyncWebServerRequest *req, const Logger *logger) {
    logger->logDebug(
        ("MBX Server: Started processing "
         + String(req->methodToString()) + " request on " + req->url()).c_str());

    JsonDocument doc;
    doc = StatService::appendSystemStats(doc, logger);
    doc = StatService::appendModbusStats(doc);
    doc = StatService::appendMQTTStats(doc);
    doc = StatService::appendNetworkStats(doc);
    doc = StatService::appendStorageStats(doc);
    doc = StatService::appendHealthStats(doc);

    sendJson(req, doc);
    logger->logDebug(
        ("MBX Server: Finished processing " + String(req->methodToString()) + " request on " + req->url()).c_str());
}

void MBXServerHandlers::getLogs(AsyncWebServerRequest *req) {
    if (auto *mem = g_memlog.load(std::memory_order_acquire)) {
        constexpr size_t MAX_LOG_BYTES = 8192;
        const size_t totalSize = mem->flattenedSize();
        const size_t offset = (totalSize > MAX_LOG_BYTES) ? (totalSize - MAX_LOG_BYTES) : 0;
        const size_t payloadSize = totalSize - offset;

        auto filler = [mem, offset, payloadSize](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= payloadSize || maxLen == 0) {
                return 0;
            }
            const size_t remaining = payloadSize - index;
            const size_t chunk = (remaining < maxLen) ? remaining : maxLen;
            return mem->copyAsText(offset + index, buffer, chunk);
        };

        auto *response = req->beginResponse("text/plain; charset=utf-8", payloadSize, filler);
        response->addHeader("Cache-Control", "no-store");
        response->addHeader("X-Log-Truncated", offset > 0 ? "true" : "false");
        req->send(response);
    } else {
        req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::PLAIN_TEXT, "logging buffer unavailable");
    }
}

void MBXServerHandlers::handleMqttTestConnection(AsyncWebServerRequest *req) {
    auto *link = MBXServerHandlers::getMqttManager();
    JsonDocument doc;
    if (!link) {
        doc["ok"] = false;
        doc["error"] = "mqtt_unavailable";
        String out;
        serializeJson(doc, out);
        req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::JSON, out);
        return;
    }

    if (!scheduleRuntimeJob(req, [link](RuntimeResponse &runtimeResponse) {
            const MqttTestResult result = link->testConnectOnce();
            JsonDocument response;
            response["ok"] = result.connected;
            response["broker"] = result.broker;
            response["user"] = result.user;
            response["state"] = result.clientState;
            response["operation"] = mqttOperationResultToString(result.operation);
            setRuntimeJsonResponse(runtimeResponse, response);
        })) {
        doc["ok"] = false;
        doc["error"] = "runtime_queue_full";
        sendJson(req, doc);
    }
}

void MBXServerHandlers::handleModbusExecute(AsyncWebServerRequest *req) {
    auto getParam = [&](const char *name, String &out) -> bool {
        if (!req->hasParam(name)) return false;
        out = req->getParam(name)->value();
        return out.length() > 0;
    };

    String devId, dpId, sFunc, sAddr, sLen;
    if (!getParam("devId", devId) || !getParam("dpId", dpId)
        || !getParam("func_code", sFunc) || !getParam("addr", sAddr)
        || !getParam("len", sLen)) {
        req->send(HttpResponseCodes::BAD_REQUEST, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        return;
    }
    String sSlave;
    const bool hasSlaveOverride = getParam("slave", sSlave);

    const long func = sFunc.toInt();
    const long addr = sAddr.toInt();
    long len = sLen.toInt();
    const long slaveOverride = hasSlaveOverride ? sSlave.toInt() : 0;
    const bool slaveOverrideValid = slaveOverride > 0 && slaveOverride <= 247;
    if (func <= 0 || addr < 0 || len <= 0) {
        req->send(HttpResponseCodes::BAD_REQUEST, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        return;
    }

    // Optional value for write operations (ignored here but echoed back)
    String sValue;
    if (req->hasParam("value")) sValue = req->getParam("value")->value();

    // Execute against Modbus
    JsonDocument doc;
    ModbusManager *mb = getModbusManager();
    if (!mb) {
        doc["ok"] = false;
        doc["error"] = "modbus_unavailable";
        sendJson(req, doc);
        return;
    }

    uint16_t writeVal = 0;
    bool hasWriteVal = false;
    if (sValue.length()) {
        if (sValue.equalsIgnoreCase("true") || sValue == "1") writeVal = 1;
        else if (sValue.equalsIgnoreCase("false") || sValue == "0") writeVal = 0;
        else writeVal = static_cast<uint16_t>(sValue.toInt());
        hasWriteVal = true;
    }

    if (func == 5 || func == 6 || func == 16) {
        len = 1; // single write workaround even for FC16
    }

    ModbusCommandRequest command;
    command.deviceId = devId;
    command.datapointId = dpId;
    command.slaveId = slaveOverrideValid ? static_cast<uint8_t>(slaveOverride) : 0U;
    command.hasSlaveOverride = slaveOverrideValid;
    command.function = static_cast<int>(func);
    command.address = static_cast<uint16_t>(addr);
    command.length = static_cast<uint16_t>(len);
    command.writeValue = writeVal;
    command.hasWriteValue = hasWriteVal;
    if (!scheduleRuntimeJob(req,
            [mb, command, devId, dpId, sValue, func, addr, len]
            (RuntimeResponse &runtimeResponse) {
                const ModbusCommandResult result = mb->executeCommand(command);
                JsonDocument response;
                if (result.operation != ModbusOperationResult::Success) {
                    response["ok"] = false;
                    response["error"] = "modbus_command_failed";
                    response["reason"] = modbusOperationResultToString(result.operation);
                    setRuntimeJsonResponse(runtimeResponse, response, HttpResponseCodes::SERVICE_UNAVAILABLE);
                    return;
                }

                response["ok"] = result.busStatus == 0;
                response["code"] = result.busStatus;
                response["state"] = ModbusManager::statusToString(result.busStatus);
                response["devId"] = devId;
                response["dpId"] = dpId;
                response["request"]["func_code"] = func;
                response["request"]["addr"] = addr;
                response["request"]["len"] = len;
                if (sValue.length()) response["request"]["value"] = sValue;
                if (result.rxDump.length()) response["rx_dump"] = result.rxDump;
                if (result.count > 0U) {
                    JsonArray raw = response["result"]["raw"].to<JsonArray>();
                    for (uint16_t i = 0; i < result.count; ++i) (void)raw.add(result.words[i]);
                    if (result.hasDatapointMetadata && result.dataType == TEXT) {
                        response["result"]["value"] =
                            ModbusManager::registersToAscii(result.words.data(), result.count);
                    } else if (result.hasDatapointMetadata) {
                        const uint16_t sliced = ModbusManager::sliceRegister(
                            result.words[0], result.registerSlice);
                        response["result"]["value"] = static_cast<float>(sliced) * result.scale;
                    } else {
                        response["result"]["value"] = result.words[0];
                    }
                }
                setRuntimeJsonResponse(runtimeResponse, response);
            })) {
        doc["ok"] = false;
        doc["error"] = "runtime_queue_full";
        sendJson(req, doc);
    }
}

void MBXServerHandlers::handleDeviceReset(AsyncWebServerRequest *req, const Logger *logger) {
    if (req == nullptr) return;

    switch (RebootScheduler::schedule()) {
        case RebootScheduler::ScheduleResult::Accepted:
            if (logger) logger->logInformation("Device reboot scheduled");
            req->send(HttpResponseCodes::ACCEPTED, HttpMediaTypes::JSON, REBOOT_ACCEPTED_RESP);
            return;
        case RebootScheduler::ScheduleResult::Pending:
            req->send(HttpResponseCodes::CONFLICT, HttpMediaTypes::JSON, REBOOT_PENDING_RESP);
            return;
        case RebootScheduler::ScheduleResult::Unavailable:
            if (logger) logger->logError("Device reboot scheduling failed: task unavailable");
            req->send(HttpResponseCodes::SERVICE_UNAVAILABLE, HttpMediaTypes::JSON, REBOOT_UNAVAILABLE_RESP);
            return;
    }
}

bool MBXServerHandlers::isOtaRequestAuthorized(const AsyncWebServerRequest *req) {
    if (!OtaCredentialService::isProtected()) return true;
    if (req == nullptr || req->authType() != AsyncAuthType::AUTH_BEARER) return false;
    const String &encoded = req->authChallenge();
    return OtaAuthorizationCore::authorize(
        true, encoded.c_str(), encoded.length(), verifyOtaPassword, nullptr);
}

bool MBXServerHandlers::cacheOtaUploadAuthorization(AsyncWebServerRequest *req) {
    if (req == nullptr || req->_tempObject != nullptr) return false;

    auto *state = static_cast<OtaUploadState *>(calloc(1U, sizeof(OtaUploadState)));
    if (state == nullptr) return false;
    state->magic = OTA_UPLOAD_STATE_MAGIC;
    req->_tempObject = state;
    req->setAttribute(OTA_UPLOAD_STATE_ATTRIBUTE, true);
    return OtaUploadGuardCore::authorize(state->guard, authorizeOtaRequest, req);
}

void MBXServerHandlers::sendOtaUnauthorized(AsyncWebServerRequest *req) {
    if (req == nullptr) return;
    AsyncWebServerResponse *response = req->beginResponse(
        HttpResponseCodes::UNAUTHORIZED, HttpMediaTypes::JSON, OTA_UNAUTHORIZED_RESP);
    response->addHeader("WWW-Authenticate", "Bearer realm=\"ota\"");
    response->addHeader("Cache-Control", "no-store");
    req->send(response);
}

void MBXServerHandlers::sendMutationForbidden(AsyncWebServerRequest *req) {
    if (req == nullptr) return;
    AsyncWebServerResponse *response = req->beginResponse(
        HttpResponseCodes::FORBIDDEN, HttpMediaTypes::JSON, MutationRequestGuardCore::FORBIDDEN_RESPONSE);
    response->addHeader("Cache-Control", "no-store");
    req->send(response);
}

void MBXServerHandlers::handleOtaFirmwareUpload(AsyncWebServerRequest *r, const String &fn, const size_t index,
                                                uint8_t *data, const size_t len, const bool final,
                                                const Logger *logger) {
    if (index == 0U) {
        OtaUploadState *state = beginOtaUploadRequest(r);
        if (state == nullptr || !state->guard.authorized) return;
        if (logger) {
            logger->logInformation((String("OTA firmware upload start: ") + fn).c_str());
        }
        OtaOperationContext operation{logger, true};
        const auto start = OtaUploadGuardCore::start(
            state->guard, beginOtaOperation, &operation);
        if (start != OtaUploadGuardCore::StartResult::Started) {
            if (logger) {
                logger->logError("OTA begin firmware failed");
            }
            r->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, OTA_FW_UPLOAD_BEGIN_FAIL_RESP);
            return;
        }
    }
    OtaUploadState *state = otaUploadState(r);
    if (state == nullptr || !state->guard.authorized || !state->guard.begun) return;
    if (len) {
        OtaOperationContext operation{logger, true};
        if (!OtaUploadGuardCore::write(state->guard, writeOtaOperation, &operation, data, len)) {
            if (logger) logger->logError("OTA firmware write failed");
        }
    }
    if (final) {
        const bool ok = OtaService::end(true, logger);
        if (!ok) {
            r->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON,
                    OTA_END_FAIL_RESP);
        } else {
            r->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, OTA_END_FW_UPLOAD_OK);
            xTaskCreatePinnedToCore([](void *) {
                delay(500);
                ESP.restart();
            }, "otaReboot", 2048, nullptr, 1, nullptr, APP_CPU_NUM);
        }
    }
}

void MBXServerHandlers::handleOtaFilesystemUpload(AsyncWebServerRequest *r, const String &fn, const size_t index,
                                                  uint8_t *data, const size_t len, const bool final,
                                                  const Logger *logger) {
    if (index == 0U) {
        OtaUploadState *state = beginOtaUploadRequest(r);
        if (state == nullptr || !state->guard.authorized) return;
        if (logger) logger->logInformation((String("OTA filesystem upload start: ") + fn).c_str());
        OtaOperationContext operation{logger, false};
        const auto start = OtaUploadGuardCore::start(
            state->guard, beginOtaOperation, &operation);
        if (start != OtaUploadGuardCore::StartResult::Started) {
            if (logger) logger->logError("OTA begin fs failed");
            r->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, OTA_FS_UPLOAD_BEGIN_FAIL_RESP);
            return;
        }
    }
    OtaUploadState *state = otaUploadState(r);
    if (state == nullptr || !state->guard.authorized || !state->guard.begun) return;
    if (len) {
        OtaOperationContext operation{logger, false};
        if (!OtaUploadGuardCore::write(state->guard, writeOtaOperation, &operation, data, len)) {
            if (logger) logger->logError("OTA fs write failed");
        }
    }
    if (final) {
        const bool ok = OtaService::end(true, logger);
        if (!ok) {
            r->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, OTA_END_FAIL_RESP);
        } else {
            r->send(HttpResponseCodes::OK, HttpMediaTypes::JSON, OTA_END_FS_UPLOAD_OK);
            xTaskCreatePinnedToCore([](void *) {
                delay(500);
                ESP.restart();
            }, "otaReboot", 2048, nullptr, 1, nullptr, APP_CPU_NUM);
        }
    }
}

void MBXServerHandlers::handleOtaHttpCheck(AsyncWebServerRequest *req, const Logger *logger) {
    (void)logger;
#if OTA_HTTP_ENABLED
    bool refresh = true;
    if (req->hasParam("refresh")) {
        const String value = req->getParam("refresh")->value();
        refresh = !(value == "0" || value.equalsIgnoreCase("false"));
    }
    if (refresh) {
        HttpOtaService::checkNow();
    }

    bool ok = false;
    bool available = false;
    String version;
    String error;
    HttpOtaService::getLastCheckStatus(ok, available, version, error);
    const bool pending = HttpOtaService::isCheckPending();

    JsonDocument doc;
    doc["ok"] = ok;
    doc["available"] = available;
    doc["pending"] = pending;
    if (available && version.length()) doc["version"] = version;
    if (!pending && !ok && error.length()) doc["error"] = error;
    sendJson(req, doc);
#else
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "ota_http_disabled";
    sendJson(req, doc);
#endif
}

void MBXServerHandlers::handleOtaHttpNotes(AsyncWebServerRequest *req, const Logger *logger) {
    (void)logger;
#if OTA_HTTP_ENABLED
    String version;
    if (!HttpOtaService::hasPendingUpdate(version)) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["error"] = "no_update";
        sendJson(req, doc);
        return;
    }

    bool refresh = true;
    if (req->hasParam("refresh")) {
        const String value = req->getParam("refresh")->value();
        refresh = !(value == "0" || value.equalsIgnoreCase("false"));
    }
    if (refresh) {
        HttpOtaService::requestReleaseNotes();
    }

    bool ready = false;
    bool pending = false;
    String notes;
    String error;
    HttpOtaService::getNotesStatus(ready, pending, notes, error);

    JsonDocument doc;
    doc["ok"] = ready || pending;
    doc["pending"] = pending;
    doc["available"] = true;
    if (version.length()) doc["version"] = version;
    if (ready) {
        doc["notes"] = notes;
    } else if (!pending && error.length()) {
        doc["ok"] = false;
        doc["error"] = error;
    }
    sendJson(req, doc);
#else
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "ota_http_disabled";
    sendJson(req, doc);
#endif
}

void MBXServerHandlers::handleOtaHttpApply(AsyncWebServerRequest *req, const Logger *logger) {
    (void)logger;
#if OTA_HTTP_ENABLED
    JsonDocument doc;
    String version;
    if (!HttpOtaService::hasPendingUpdate(version)) {
        doc["ok"] = false;
        doc["error"] = "no_update";
        sendJson(req, doc);
        return;
    }

    if (g_otaHttpApplying.exchange(true)) {
        doc["ok"] = false;
        doc["error"] = "ota_in_progress";
        sendJson(req, doc);
        return;
    }

    doc["ok"] = true;
    doc["started"] = true;
    if (version.length()) doc["version"] = version;
    sendJson(req, doc);

    auto *loggerPtr = const_cast<Logger *>(logger);
    xTaskCreatePinnedToCore([](void *param) {
        auto *log = static_cast<Logger *>(param);
        if (log) log->logInformation("HTTP-OTA: Apply started");
        String error;
        const bool ok = HttpOtaService::applyPendingUpdate(error);
        if (!ok) {
            const String msg = "HTTP-OTA: Apply failed: " + error;
            if (log) log->logError(msg.c_str());
            Serial.println(msg);
            g_otaHttpApplying.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }
        if (log) log->logInformation("HTTP-OTA: Apply complete, rebooting");
        Serial.println("HTTP-OTA: Apply complete, rebooting");
        sendOtaStatus("rebooting", 0, 0, nullptr);
        g_otaHttpApplying.store(false, std::memory_order_release);
        delay(500);
        ESP.restart();
    }, "otaHttpApply", 8192, loggerPtr, 1, nullptr, APP_CPU_NUM);
#else
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "ota_http_disabled";
    sendJson(req, doc);
#endif
}

void MBXServerHandlers::handleGetOtaHttpSettings(AsyncWebServerRequest *req) {
#if OTA_HTTP_ENABLED
    JsonDocument doc;
    doc["includePrereleases"] = HttpOtaService::getIncludePrereleases();
    doc["passwordProtected"] = OtaCredentialService::isProtected();
    sendJson(req, doc);
#else
    JsonDocument doc;
    doc["error"] = "ota_http_disabled";
    sendJson(req, doc);
#endif
}

void MBXServerHandlers::handlePutOtaHttpSettingsBody(AsyncWebServerRequest *req, const uint8_t *data, const size_t len,
                                                    const size_t index, const size_t total) {
#if OTA_HTTP_ENABLED
    char *body = BodyAccumulator::append(req->_tempObject, data, len, index, total);
    if (body == nullptr) {
        if (index + len == total) {
            logHandlerError("POST /api/system/ota/http/settings: out of memory accumulating request body");
            req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        }
        return;
    }

    JsonDocument doc;
    const DeserializationError derr = deserializeJson(doc, body, total);
    if (derr || !doc["includePrereleases"].is<bool>()) {
        req->send(HttpResponseCodes::BAD_REQUEST, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        return;
    }
    HttpOtaService::setIncludePrereleases(doc["includePrereleases"].as<bool>());
    req->send(HttpResponseCodes::NO_CONTENT);
#else
    (void)data; (void)len; (void)index; (void)total;
    JsonDocument doc;
    doc["error"] = "ota_http_disabled";
    sendJson(req, doc);
#endif
}

void MBXServerHandlers::handlePutOtaPasswordBody(AsyncWebServerRequest *req, const uint8_t *data, const size_t len,
                                                 const size_t index, const size_t total) {
    char *body = BodyAccumulator::append(req->_tempObject, data, len, index, total);
    if (body == nullptr) {
        if (index + len == total) {
            req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        }
        return;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body, total);
    if (error || !doc["password"].is<const char *>()) {
        req->send(HttpResponseCodes::BAD_REQUEST_HTTP, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        return;
    }

    const String password = doc["password"].as<String>();
    switch (OtaCredentialService::setPassword(password)) {
        case OtaCredentialService::SaveResult::Ok:
            req->send(HttpResponseCodes::NO_CONTENT);
            return;
        case OtaCredentialService::SaveResult::InvalidPassword:
            req->send(HttpResponseCodes::BAD_REQUEST_HTTP, HttpMediaTypes::JSON, OTA_PASSWORD_INVALID_RESP);
            return;
        case OtaCredentialService::SaveResult::StorageError:
            req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, OTA_PASSWORD_STORAGE_RESP);
            return;
    }
}

void MBXServerHandlers::handleDeleteOtaPassword(AsyncWebServerRequest *req) {
    if (!OtaCredentialService::clearPassword()) {
        req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, OTA_PASSWORD_STORAGE_RESP);
        return;
    }
    req->send(HttpResponseCodes::NO_CONTENT);
}

void MBXServerHandlers::handleFactoryResetBody(AsyncWebServerRequest *req, const uint8_t *data, const size_t len,
                                               const size_t index, const size_t total, const Logger *logger) {
    char *body = BodyAccumulator::append(req->_tempObject, data, len, index, total);
    if (body == nullptr) {
        if (index + len == total) {
            req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        }
        return;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body, total);
    const String confirmation = doc["confirm"] | "";
    if (error || confirmation != "factory-reset") {
        req->send(HttpResponseCodes::BAD_REQUEST_HTTP, HttpMediaTypes::JSON, BAD_REQUEST_RESP);
        return;
    }

    if (g_factoryResetInProgress.exchange(true, std::memory_order_acq_rel)) {
        req->send(HttpResponseCodes::CONFLICT, HttpMediaTypes::JSON, FACTORY_RESET_BUSY_RESP);
        return;
    }

    auto *context = new (std::nothrow) FactoryResetContext;
    if (context == nullptr) {
        g_factoryResetInProgress.store(false, std::memory_order_release);
        if (logger) logger->logError("Factory reset failed: task context allocation failed");
        req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, FACTORY_RESET_TASK_RESP);
        return;
    }
    context->request = req->pause();
    context->logger = logger;

    if (FactoryResetCore::start(scheduleFactoryResetTask, context)
        == FactoryResetCore::StartResult::TaskUnavailable) {
        delete context;
        g_factoryResetInProgress.store(false, std::memory_order_release);
        if (logger) logger->logError("Factory reset failed: task creation failed");
        req->send(HttpResponseCodes::INTERNAL_SERVER_ERROR, HttpMediaTypes::JSON, FACTORY_RESET_TASK_RESP);
        return;
    }
    if (logger) logger->logWarning("Factory reset requested; clearing all persistent configuration");
}
