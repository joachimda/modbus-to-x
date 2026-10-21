// Native-host tests for BodyAccumulator and other platform-neutral units.
//
// BodyAccumulator has no Arduino dependencies, so we include its
// translation unit directly to avoid configuring test_build_src /
// build_src_filter just for one file.

#include "../src/network/mbx_server/BodyAccumulator.cpp"
#include "../src/mqtt/MqttConfigCore.cpp"
#include "../src/mqtt/MqttConfigDocument.cpp"
#include "../src/mqtt/MqttConfigMutationCore.cpp"
#include "../src/modbus/ModbusMqttWriteCore.cpp"
#include "../include/concurrency/OwnerCompletionCore.h"
#include "../include/concurrency/OwnerMailboxCore.h"
#include "../include/concurrency/LatestRevisionCore.h"
#include "../include/concurrency/PublicationAttemptCore.h"
#include "../include/concurrency/StagedCommitCore.h"
#include "../include/concurrency/CoherentSnapshotCore.h"
#include "../include/mqtt/MqttOwnerCore.h"
#include "../include/modbus/ModbusOwnerCore.h"
#include "../include/network/mbx_server/MutationRequestGuardCore.h"
#include "../include/network/mbx_server/MutationRouteRegistration.h"
#include "../include/network/mbx_server/OtaRouteRegistration.h"
#include "../include/network/wifi/ProvisioningAttemptCore.h"
#include "../include/network/wifi/ProvisioningCompletionCore.h"
#include "../include/network/wifi/ProvisioningEventGate.h"
#include "../include/network/wifi/WifiCredentialFieldCore.h"
#include "../include/services/FactoryResetCore.h"
#include "../include/services/RebootSchedulerCore.h"
#include "../src/services/ota/OtaCredentialCore.cpp"
#include "../src/services/ota/OtaAuthorizationCore.cpp"
#include "../src/services/ota/OtaCredentialService.cpp"
#include "../src/network/wifi/WifiConfigurationStorage.cpp"
#include "../include/services/ota/OtaUploadGuardCore.h"
#include "../include/services/ota/OtaAdmissionGateCore.h"
#include "../include/services/ota/OtaQuiescenceCore.h"
#include "../include/services/ota/OtaProgressCore.h"
#include "../include/services/ota/OtaSessionCore.h"
#include "../include/Config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <utility>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <Preferences.h>
#include <unity.h>

#include "../src/utils/StringUtils.cpp"
#include "../src/modbus/ModbusValueCodec.cpp"
#include "../src/modbus/ModbusConfigDocument.cpp"
#include "../src/modbus/ModbusTopicBuilder.cpp"
#include "../src/modbus/RtuFrame.cpp"
#include "../src/modbus/RtuTransport.cpp"
#include "../src/modbus/WavinFrame.cpp"
#include "../src/modbus/WavinClient.cpp"
#include "../src/modbus/ProtocolProfile.cpp"
#include "../include/modbus/RtuLink.h"

namespace {

uint8_t g_randomSeed = 1;
int g_deriveCalls = 0;

struct FactoryResetFake {
    bool scheduleResult = true;
    bool formatResult = true;
    bool eraseResult = true;
    int scheduleCalls = 0;
    int formatCalls = 0;
    int eraseCalls = 0;
};

struct RebootSchedulerFake {
    bool scheduleResult = true;
    int scheduleCalls = 0;
    int waitCalls = 0;
    int restartCalls = 0;
    uint32_t waitedMs = 0U;
    RebootSchedulerCore::Task task = nullptr;
    void *taskContext = nullptr;
};

bool fakeScheduleRebootTask(const RebootSchedulerCore::Task task, void *taskContext,
                            void *scheduleContext) {
    auto *fake = static_cast<RebootSchedulerFake *>(scheduleContext);
    ++fake->scheduleCalls;
    fake->task = task;
    fake->taskContext = taskContext;
    return fake->scheduleResult;
}

void fakeWaitForReboot(const uint32_t delayMs, void *executionContext) {
    auto *fake = static_cast<RebootSchedulerFake *>(executionContext);
    ++fake->waitCalls;
    fake->waitedMs = delayMs;
}

void fakeRestartDevice(void *executionContext) {
    auto *fake = static_cast<RebootSchedulerFake *>(executionContext);
    ++fake->restartCalls;
}

RebootSchedulerCore::Operations fakeRebootOperations(RebootSchedulerFake &fake) {
    return {
        fakeScheduleRebootTask,
        &fake,
        fakeWaitForReboot,
        fakeRestartDevice,
        &fake,
        1000U,
    };
}

bool fakeScheduleResetTask(void *context) {
    auto *fake = static_cast<FactoryResetFake *>(context);
    ++fake->scheduleCalls;
    return fake->scheduleResult;
}

bool fakeFormatConfigStorage(void *context) {
    auto *fake = static_cast<FactoryResetFake *>(context);
    ++fake->formatCalls;
    return fake->formatResult;
}

bool fakeEraseNvsStorage(void *context) {
    auto *fake = static_cast<FactoryResetFake *>(context);
    ++fake->eraseCalls;
    return fake->eraseResult;
}

FactoryResetCore::RunResult executeFakeFactoryReset(FactoryResetFake &fake) {
    const FactoryResetCore::Operations operations{
        fakeFormatConfigStorage,
        fakeEraseNvsStorage,
    };
    return FactoryResetCore::execute(operations, &fake);
}

bool fakeRandom(uint8_t *output, const size_t outputLength) {
    for (size_t i = 0; i < outputLength; ++i) output[i] = g_randomSeed++;
    return true;
}

bool fakeDerive(const uint8_t *password, const size_t passwordLength,
                const uint8_t *salt, const size_t saltLength,
                const uint32_t iterations, uint8_t *output, const size_t outputLength) {
    ++g_deriveCalls;
    if (password == nullptr || passwordLength == 0 || iterations == 0) return false;
    for (size_t i = 0; i < outputLength; ++i) {
        output[i] = password[i % passwordLength] ^ salt[i % saltLength]
                    ^ static_cast<uint8_t>(iterations >> ((i % 4U) * 8U));
    }
    return true;
}

enum RouteTestMethod {
    ROUTE_TEST_GET,
    ROUTE_TEST_POST,
    ROUTE_TEST_PUT,
    ROUTE_TEST_DELETE,
    ROUTE_TEST_HEAD,
};

struct RouteTestState {
    int mode = 0;
    int unauthorizedCalls = 0;
    int forbiddenCalls = 0;
    int firmwareBeginCalls = 0;
    int filesystemBeginCalls = 0;
    int writeCalls = 0;
    int checkCalls = 0;
    int notesCalls = 0;
    int applyCalls = 0;
    int getSettingsCalls = 0;
    int putSettingsCalls = 0;
    int passwordHandlerCalls = 0;
    int deletePasswordCalls = 0;
    int factoryResetHandlerCalls = 0;
    int factoryResetScheduleCalls = 0;
    int factoryResetFormatCalls = 0;
    int factoryResetEraseCalls = 0;
    int mutationSideEffectCalls = 0;
    int rebootScheduleCalls = 0;
    int otaBusyCalls = 0;
    int sessionCalls = 0;
    int passwordVerificationCalls = 0;
    int statusCalls = 0;
    int manualStartCalls = 0;
    bool otaActive = false;
    std::string sessionToken;
};

struct RouteTestRequest {
    RouteTestMethod requestMethod;
    std::string requestUrl;
    std::string bearer;
    OtaUploadGuardCore::State uploadState{};
    RouteTestState *state = nullptr;
    std::string marker = MutationRequestGuardCore::REQUEST_HEADER_VALUE;
    std::string host = "192.168.1.42";
    std::string session;

    RouteTestMethod method() const {
        return requestMethod;
    }

    const std::string &url() const {
        return requestUrl;
    }
};

struct RouteTestEntry {
    using RequestHandler = std::function<void(RouteTestRequest *)>;
    using UploadHandler = std::function<void(RouteTestRequest *, const std::string &, size_t, uint8_t *, size_t, bool)>;
    using BodyHandler = std::function<void(RouteTestRequest *, const uint8_t *, size_t, size_t, size_t)>;
    using Filter = std::function<bool(RouteTestRequest *)>;

    std::string path;
    RouteTestMethod method;
    RequestHandler requestHandler;
    UploadHandler uploadHandler;
    BodyHandler bodyHandler;
    Filter filter;

    RouteTestEntry &setFilter(Filter value) {
        filter = std::move(value);
        return *this;
    }
};

class RouteTestServer {
public:
    RouteTestServer() {
        routes.reserve(20U);
    }

    RouteTestEntry &on(const char *path, const RouteTestMethod method,
                       RouteTestEntry::RequestHandler requestHandler) {
        routes.push_back({path, method, std::move(requestHandler), nullptr, nullptr, nullptr});
        return routes.back();
    }

    RouteTestEntry &on(const char *path, const RouteTestMethod method,
                       RouteTestEntry::RequestHandler requestHandler, RouteTestEntry::UploadHandler uploadHandler) {
        routes.push_back({path, method, std::move(requestHandler), std::move(uploadHandler), nullptr, nullptr});
        return routes.back();
    }

    RouteTestEntry &on(const char *path, const RouteTestMethod method,
                       RouteTestEntry::RequestHandler requestHandler, std::nullptr_t,
                       RouteTestEntry::BodyHandler bodyHandler) {
        routes.push_back({path, method, std::move(requestHandler), nullptr, std::move(bodyHandler), nullptr});
        return routes.back();
    }

    bool dispatchRequest(RouteTestRequest &request) {
        RouteTestEntry *route = matchingRoute(request);
        if (route == nullptr || !route->requestHandler) return false;
        route->requestHandler(&request);
        return true;
    }

    bool dispatchUploadChunk(RouteTestRequest &request, uint8_t *data, const size_t length) {
        RouteTestEntry *route = matchingRoute(request);
        if (route == nullptr) return false;
        if (!route->uploadHandler) {
            if (!route->requestHandler) return false;
            route->requestHandler(&request);
            return true;
        }
        route->uploadHandler(&request, "image.bin", 0U, data, length, false);
        return true;
    }

    bool dispatchBody(RouteTestRequest &request, const char *body) {
        RouteTestEntry *route = matchingRoute(request);
        if (route == nullptr) return false;
        if (!route->bodyHandler) {
            if (!route->requestHandler) return false;
            route->requestHandler(&request);
            return true;
        }
        const size_t length = std::strlen(body);
        route->bodyHandler(&request, reinterpret_cast<const uint8_t *>(body), length, 0U, length);
        return true;
    }

    size_t count(const char *path, const RouteTestMethod method) const {
        size_t result = 0;
        for (const auto &route : routes) {
            if (route.path == path && route.method == method) ++result;
        }
        return result;
    }

private:
    RouteTestEntry *matchingRoute(RouteTestRequest &request) {
        for (auto &route : routes) {
            if (route.path != request.requestUrl || route.method != request.requestMethod) continue;
            if (route.filter && !route.filter(&request)) continue;
            return &route;
        }
        return nullptr;
    }

    std::vector<RouteTestEntry> routes;
};

bool verifyPersistedPassword(const uint8_t *password, const size_t passwordLength, void *) {
    char terminated[OtaCredentialCore::MAX_PASSWORD_SIZE + 1U]{};
    if (passwordLength > OtaCredentialCore::MAX_PASSWORD_SIZE) return false;
    std::memcpy(terminated, password, passwordLength);
    return OtaCredentialService::verify(String(terminated));
}

bool authorizeRouteRequest(const RouteTestRequest &request) {
    return OtaAuthorizationCore::authorize(
        OtaCredentialService::isProtected(), request.bearer.c_str(), request.bearer.length(),
        verifyPersistedPassword, nullptr);
}

bool returnAuthorizationResult(void *context) {
    return context != nullptr && *static_cast<const bool *>(context);
}

struct RouteUploadOperation {
    RouteTestState *state;
    bool firmware;
};

bool beginRouteUpload(void *context) {
    auto *operation = static_cast<RouteUploadOperation *>(context);
    if (operation->firmware) ++operation->state->firmwareBeginCalls;
    else ++operation->state->filesystemBeginCalls;
    return true;
}

bool writeRouteUpload(void *context, uint8_t *, size_t) {
    auto *operation = static_cast<RouteUploadOperation *>(context);
    ++operation->state->writeCalls;
    return true;
}

bool scheduleRouteFactoryReset(void *context) {
    auto *state = static_cast<RouteTestState *>(context);
    ++state->factoryResetScheduleCalls;
    return true;
}

bool formatRouteFactoryReset(void *context) {
    auto *state = static_cast<RouteTestState *>(context);
    ++state->factoryResetFormatCalls;
    return true;
}

bool eraseRouteFactoryReset(void *context) {
    auto *state = static_cast<RouteTestState *>(context);
    ++state->factoryResetEraseCalls;
    return OtaCredentialService::clearPassword();
}

struct RouteTestCallbacks {
    RouteTestState *state;

    bool isAuthorized(const RouteTestRequest *request) const {
        return request != nullptr && !state->sessionToken.empty()
               && request->session == state->sessionToken;
    }

    bool isMutationAllowed(const RouteTestRequest *request) const {
        return request != nullptr && MutationRequestGuardCore::isAllowed(
            request->marker.c_str(), request->host.c_str(), "192.168.1.42", "192.168.4.1", "modbus-to-x");
    }

    bool isOrdinaryMutationAllowed(const RouteTestRequest *) const {
        return !state->otaActive;
    }

    bool cacheUploadAuthorization(RouteTestRequest *request) const {
        if (request == nullptr || request->uploadState.authorizationChecked) return false;
        bool authorized = isAuthorized(request);
        return OtaUploadGuardCore::authorize(
            request->uploadState, returnAuthorizationResult, &authorized);
    }

    void sendUnauthorized(RouteTestRequest *) const {
        ++state->unauthorizedCalls;
    }

    void sendForbidden(RouteTestRequest *) const {
        ++state->forbiddenCalls;
    }

    void sendOtaInProgress(RouteTestRequest *) const { ++state->otaBusyCalls; }

    void sendOtaBusy(RouteTestRequest *) const { ++state->otaBusyCalls; }

    void logRequest(const RouteTestRequest *) const {}

    void handleFirmwareUpload(RouteTestRequest *request, const std::string &, const size_t,
                              uint8_t *data, const size_t length, const bool) const {
        handleUpload(request, data, length, true);
    }

    void handleFilesystemUpload(RouteTestRequest *request, const std::string &, const size_t,
                                uint8_t *data, const size_t length, const bool) const {
        handleUpload(request, data, length, false);
    }

    void handleHttpCheck(RouteTestRequest *) const { ++state->checkCalls; }
    void handleHttpNotes(RouteTestRequest *) const { ++state->notesCalls; }
    void handleHttpApply(RouteTestRequest *) const { ++state->applyCalls; }
    void handleCreateSession(RouteTestRequest *request) const {
        ++state->sessionCalls;
        if (OtaCredentialService::isProtected()) ++state->passwordVerificationCalls;
        if (!authorizeRouteRequest(*request)) {
            ++state->unauthorizedCalls;
            return;
        }
        state->sessionToken = "test-session-token";
    }
    void handleStatus(RouteTestRequest *) const { ++state->statusCalls; }
    void handleManualStartBody(RouteTestRequest *, const uint8_t *, const size_t,
                               const size_t, const size_t) const {
        ++state->manualStartCalls;
    }
    void handleGetHttpSettings(RouteTestRequest *) const { ++state->getSettingsCalls; }

    void handlePutHttpSettingsBody(RouteTestRequest *, const uint8_t *, const size_t,
                                   const size_t, const size_t) const {
        ++state->putSettingsCalls;
    }

    void handlePutPasswordBody(RouteTestRequest *, const uint8_t *data, const size_t length,
                               const size_t index, const size_t total) const {
        ++state->passwordHandlerCalls;
        if (index != 0U || length != total) return;
        const std::string body(reinterpret_cast<const char *>(data), length);
        if (body == R"({"password":"route-password"})") {
            (void)OtaCredentialService::setPassword(String("route-password"));
            state->sessionToken.clear();
        }
    }

    void handleDeletePassword(RouteTestRequest *) const {
        ++state->deletePasswordCalls;
        (void)OtaCredentialService::clearPassword();
        state->sessionToken.clear();
    }

    void handleFactoryResetBody(RouteTestRequest *, const uint8_t *data, const size_t length,
                                const size_t index, const size_t total) const {
        ++state->factoryResetHandlerCalls;
        if (index != 0U || length != total
            || std::string(reinterpret_cast<const char *>(data), length) != R"({"confirm":"factory-reset"})") {
            return;
        }
        if (FactoryResetCore::start(scheduleRouteFactoryReset, state) != FactoryResetCore::StartResult::Accepted) {
            return;
        }
        const FactoryResetCore::Operations operations{formatRouteFactoryReset, eraseRouteFactoryReset};
        (void)FactoryResetCore::execute(operations, state);
    }

    void handlePutModbusConfigBody(RouteTestRequest *, const uint8_t *, const size_t,
                                   const size_t, const size_t) const {
        ++state->mutationSideEffectCalls;
    }

    void handlePutMqttConfigBody(RouteTestRequest *, const uint8_t *, const size_t,
                                 const size_t, const size_t) const {
        ++state->mutationSideEffectCalls;
    }

    void handlePutMqttSecretBody(RouteTestRequest *, const uint8_t *, const size_t,
                                 const size_t, const size_t) const {
        ++state->mutationSideEffectCalls;
    }

    void handleMqttTestConnection(RouteTestRequest *) const { ++state->mutationSideEffectCalls; }
    void handleModbusExecute(RouteTestRequest *) const { ++state->mutationSideEffectCalls; }
    void handleModbusDisable(RouteTestRequest *, bool) const { ++state->mutationSideEffectCalls; }
    void handleDeviceReset(RouteTestRequest *) const {
        ++state->mutationSideEffectCalls;
        ++state->rebootScheduleCalls;
    }

    void handleWifiConnectBody(RouteTestRequest *, const uint8_t *, const size_t,
                               const size_t, const size_t) const {
        ++state->mutationSideEffectCalls;
    }

    void handleWifiApOff(RouteTestRequest *) const { ++state->mutationSideEffectCalls; }
    void handleWifiCancel(RouteTestRequest *) const { ++state->mutationSideEffectCalls; }
    void handleNetworkReset(RouteTestRequest *) const { ++state->mutationSideEffectCalls; }

private:
    void handleUpload(RouteTestRequest *request, uint8_t *data, const size_t length, const bool firmware) const {
        RouteUploadOperation operation{state, firmware};
        if (OtaUploadGuardCore::start(request->uploadState, beginRouteUpload, &operation)
            != OtaUploadGuardCore::StartResult::Started) {
            return;
        }
        (void)OtaUploadGuardCore::write(
            request->uploadState, writeRouteUpload, &operation, data, length);
    }
};

RouteTestServer configuredRouteTestServer(RouteTestState &state) {
    RouteTestServer server;
    OtaRouteRegistration::configure<RouteTestRequest, std::string>(
        &server, RouteTestCallbacks{&state},
        ROUTE_TEST_GET, ROUTE_TEST_POST, ROUTE_TEST_PUT, ROUTE_TEST_DELETE);
    return server;
}

RouteTestServer configuredMutationTestServer(RouteTestState &state, const MutationRouteRegistration::Mode mode) {
    RouteTestServer server;
    MutationRouteRegistration::configure<RouteTestRequest>(
        &server, RouteTestCallbacks{&state}, mode, ROUTE_TEST_POST, ROUTE_TEST_PUT);
    return server;
}

std::string createRouteSession(RouteTestServer &server, RouteTestState &state,
                               const char *bearer = "") {
    RouteTestRequest request{ROUTE_TEST_POST, Routes::OTA_SESSION, bearer, {}, &state};
    if (!server.dispatchRequest(request)) return {};
    return state.sessionToken;
}

void freeSlot(void *&slot) {
    if (slot != nullptr) {
        std::free(slot);
        slot = nullptr;
    }
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Single-chunk body delivers immediately and returns the full payload.
// ---------------------------------------------------------------------------
void test_single_chunk_returns_full_buffer(void) {
    void *slot = nullptr;
    const char *payload = "hello";
    const size_t total = std::strlen(payload);

    char *body = BodyAccumulator::append(slot, reinterpret_cast<const uint8_t *>(payload),
                                         total, /*index=*/0, total);

    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_EQUAL_STRING_LEN(payload, body, total);
    TEST_ASSERT_EQUAL_UINT8(0, body[total]);  // NUL terminator preserved
    freeSlot(slot);
}

// ---------------------------------------------------------------------------
// Multi-chunk body returns nullptr until the final chunk, then full payload.
// ---------------------------------------------------------------------------
void test_multi_chunk_assembles_in_order(void) {
    void *slot = nullptr;
    const char *part1 = "{\"ssid\":";
    const char *part2 = "\"home\",";
    const char *part3 = "\"password\":\"x\"}";
    const size_t l1 = std::strlen(part1);
    const size_t l2 = std::strlen(part2);
    const size_t l3 = std::strlen(part3);
    const size_t total = l1 + l2 + l3;

    char *r1 = BodyAccumulator::append(slot, reinterpret_cast<const uint8_t *>(part1), l1, 0, total);
    TEST_ASSERT_NULL(r1);
    char *r2 = BodyAccumulator::append(slot, reinterpret_cast<const uint8_t *>(part2), l2, l1, total);
    TEST_ASSERT_NULL(r2);
    char *r3 = BodyAccumulator::append(slot, reinterpret_cast<const uint8_t *>(part3), l3, l1 + l2, total);
    TEST_ASSERT_NOT_NULL(r3);

    char expected[64];
    std::snprintf(expected, sizeof(expected), "%s%s%s", part1, part2, part3);
    TEST_ASSERT_EQUAL_STRING(expected, r3);
    freeSlot(slot);
}

// ---------------------------------------------------------------------------
// Two interleaved requests must never cross-contaminate. Each slot holds
// only its own bytes when its final chunk fires.
// ---------------------------------------------------------------------------
void test_interleaved_requests_are_independent(void) {
    void *slotA = nullptr;
    void *slotB = nullptr;

    const char *a1 = "AAAA";
    const char *a2 = "aaaaa";
    const char *b1 = "BB";
    const char *b2 = "bbbbbbb";

    const size_t la1 = std::strlen(a1), la2 = std::strlen(a2);
    const size_t lb1 = std::strlen(b1), lb2 = std::strlen(b2);
    const size_t totalA = la1 + la2;
    const size_t totalB = lb1 + lb2;

    // Interleave: A1, B1, A2 (final for A), B2 (final for B).
    TEST_ASSERT_NULL(BodyAccumulator::append(slotA, reinterpret_cast<const uint8_t *>(a1), la1, 0, totalA));
    TEST_ASSERT_NULL(BodyAccumulator::append(slotB, reinterpret_cast<const uint8_t *>(b1), lb1, 0, totalB));
    char *finalA = BodyAccumulator::append(slotA, reinterpret_cast<const uint8_t *>(a2), la2, la1, totalA);
    char *finalB = BodyAccumulator::append(slotB, reinterpret_cast<const uint8_t *>(b2), lb2, lb1, totalB);

    TEST_ASSERT_NOT_NULL(finalA);
    TEST_ASSERT_NOT_NULL(finalB);
    TEST_ASSERT_EQUAL_STRING("AAAAaaaaa", finalA);
    TEST_ASSERT_EQUAL_STRING("BBbbbbbbb", finalB);
    TEST_ASSERT_TRUE(slotA != slotB);
    freeSlot(slotA);
    freeSlot(slotB);
}

// ---------------------------------------------------------------------------
// An aborted request leaves a non-null slot behind. The next request to the
// same slot (simulating a fresh AsyncWebServerRequest reusing memory in a
// test) must reset cleanly without leaking the old buffer.
// ---------------------------------------------------------------------------
void test_aborted_then_clean_request(void) {
    void *slot = nullptr;
    const char *partial = "PARTIAL_";
    const size_t lp = std::strlen(partial);
    const size_t abortedTotal = 100;  // total advertised, but we never finish

    char *r = BodyAccumulator::append(slot, reinterpret_cast<const uint8_t *>(partial), lp, 0, abortedTotal);
    TEST_ASSERT_NULL(r);
    TEST_ASSERT_NOT_NULL(slot);  // mid-stream buffer exists

    // Simulate framework destructor freeing the slot, then a fresh request:
    freeSlot(slot);

    const char *fresh = "fresh-body";
    const size_t lf = std::strlen(fresh);
    char *r2 = BodyAccumulator::append(slot, reinterpret_cast<const uint8_t *>(fresh), lf, 0, lf);
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_EQUAL_STRING(fresh, r2);
    freeSlot(slot);
}

// ---------------------------------------------------------------------------
// Defensive cleanup: if the slot still holds a previous buffer when a new
// request begins (framework somehow skipped cleanup), append() must drop it
// rather than overwrite into the old allocation.
// ---------------------------------------------------------------------------
void test_lingering_slot_replaced_on_new_request(void) {
    void *slot = std::malloc(8);
    std::memset(slot, 'X', 8);
    const char *fresh = "Y";
    const size_t lf = std::strlen(fresh);

    char *r = BodyAccumulator::append(slot, reinterpret_cast<const uint8_t *>(fresh), lf, 0, lf);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_STRING("Y", r);
    freeSlot(slot);
}

// ---------------------------------------------------------------------------
// Empty body (Content-Length: 0): index=0, len=0, total=0. The accumulator
// must allocate a zero-length (but NUL-terminated) buffer and return it on
// the first and only invocation.
// ---------------------------------------------------------------------------
void test_empty_body_returns_empty_buffer(void) {
    void *slot = nullptr;
    char *body = BodyAccumulator::append(slot, nullptr, 0, 0, 0);
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_EQUAL_UINT8(0, body[0]);
    freeSlot(slot);
}

// ---------------------------------------------------------------------------
// Post-OOM: if index==0's allocation failed, slot stays NULL. Subsequent
// chunks (which the framework still delivers because it doesn't know the
// handler is in a failure state) must return NULL without crashing.
// ---------------------------------------------------------------------------
void test_post_oom_drains_safely(void) {
    void *slot = nullptr;  // simulates calloc() having returned NULL on chunk 0
    const char *data = "ignored";
    const size_t ld = std::strlen(data);

    char *r = BodyAccumulator::append(slot, reinterpret_cast<const uint8_t *>(data),
                                      ld, /*index=*/10, /*total=*/100);
    TEST_ASSERT_NULL(r);
    TEST_ASSERT_NULL(slot);  // still nullptr; we did not allocate mid-stream

    // Even the "final" chunk in the post-OOM state should return NULL so the
    // handler emits 500 instead of dereferencing a null buffer.
    r = BodyAccumulator::append(slot, reinterpret_cast<const uint8_t *>(data),
                                ld, /*index=*/93, /*total=*/100);
    TEST_ASSERT_NULL(r);
    TEST_ASSERT_NULL(slot);
}

void test_ota_password_length_policy(void) {
    TEST_ASSERT_FALSE(OtaCredentialCore::isPasswordLengthValid(0));
    TEST_ASSERT_FALSE(OtaCredentialCore::isPasswordLengthValid(7));
    TEST_ASSERT_TRUE(OtaCredentialCore::isPasswordLengthValid(8));
    TEST_ASSERT_TRUE(OtaCredentialCore::isPasswordLengthValid(128));
    TEST_ASSERT_FALSE(OtaCredentialCore::isPasswordLengthValid(129));
}

void test_ota_password_record_set_verify_change_and_clear(void) {
    OtaCredentialCore::Record record;
    const char *first = "first-password";
    const char *second = "second-password";
    g_randomSeed = 1;

    TEST_ASSERT_TRUE(OtaCredentialCore::createRecord(
        record, reinterpret_cast<const uint8_t *>(first), std::strlen(first), fakeRandom, fakeDerive));
    TEST_ASSERT_TRUE(record.configured);
    TEST_ASSERT_EQUAL_UINT8(OtaCredentialCore::RECORD_VERSION, record.version);
    TEST_ASSERT_EQUAL_UINT32(OtaCredentialCore::PBKDF2_ITERATIONS, record.iterations);
    TEST_ASSERT_TRUE(OtaCredentialCore::verify(
        record, reinterpret_cast<const uint8_t *>(first), std::strlen(first), fakeDerive));
    TEST_ASSERT_FALSE(OtaCredentialCore::verify(
        record, reinterpret_cast<const uint8_t *>("wrong-password"), 14, fakeDerive));

    uint8_t firstSalt[OtaCredentialCore::SALT_SIZE];
    std::memcpy(firstSalt, record.salt, sizeof(firstSalt));
    TEST_ASSERT_TRUE(OtaCredentialCore::createRecord(
        record, reinterpret_cast<const uint8_t *>(second), std::strlen(second), fakeRandom, fakeDerive));
    TEST_ASSERT_FALSE(OtaCredentialCore::constantTimeEqual(firstSalt, record.salt, sizeof(firstSalt)));
    TEST_ASSERT_FALSE(OtaCredentialCore::verify(
        record, reinterpret_cast<const uint8_t *>(first), std::strlen(first), fakeDerive));
    TEST_ASSERT_TRUE(OtaCredentialCore::verify(
        record, reinterpret_cast<const uint8_t *>(second), std::strlen(second), fakeDerive));

    OtaCredentialCore::clearRecord(record);
    TEST_ASSERT_FALSE(record.configured);
    TEST_ASSERT_FALSE(OtaCredentialCore::verify(
        record, reinterpret_cast<const uint8_t *>(second), std::strlen(second), fakeDerive));
}

void test_ota_password_supported_iteration_bounds(void) {
    OtaCredentialCore::Record record;
    record.configured = true;
    record.version = OtaCredentialCore::RECORD_VERSION;

    record.iterations = OtaCredentialCore::MIN_SUPPORTED_PBKDF2_ITERATIONS;
    TEST_ASSERT_TRUE(OtaCredentialCore::isRecordValid(record));
    record.iterations = OtaCredentialCore::MAX_SUPPORTED_PBKDF2_ITERATIONS;
    TEST_ASSERT_TRUE(OtaCredentialCore::isRecordValid(record));
    record.iterations = OtaCredentialCore::MIN_SUPPORTED_PBKDF2_ITERATIONS - 1U;
    TEST_ASSERT_FALSE(OtaCredentialCore::isRecordValid(record));
    record.iterations = OtaCredentialCore::MAX_SUPPORTED_PBKDF2_ITERATIONS + 1U;
    TEST_ASSERT_FALSE(OtaCredentialCore::isRecordValid(record));
}

void test_ota_password_unsupported_legacy_cost_fails_without_deriving(void) {
    OtaCredentialCore::Record record;
    record.configured = true;
    record.version = OtaCredentialCore::RECORD_VERSION;
    record.iterations = 100000U;
    g_deriveCalls = 0;

    TEST_ASSERT_FALSE(OtaCredentialCore::isRecordValid(record));
    TEST_ASSERT_FALSE(OtaCredentialCore::verify(
        record, reinterpret_cast<const uint8_t *>("valid-password"), 14, fakeDerive));
    TEST_ASSERT_EQUAL_INT(0, g_deriveCalls);
}

void test_ota_credential_service_persists_and_clears_record(void) {
    Preferences::resetTestStorage();

    TEST_ASSERT_FALSE(OtaCredentialService::isProtected());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(OtaCredentialService::SaveResult::Ok),
                          static_cast<int>(OtaCredentialService::setPassword(String("route-password"))));
    TEST_ASSERT_TRUE(OtaCredentialService::isProtected());
    TEST_ASSERT_TRUE(OtaCredentialService::verify(String("route-password")));
    TEST_ASSERT_FALSE(OtaCredentialService::verify(String("wrong-password")));

    Preferences persisted;
    TEST_ASSERT_TRUE(persisted.begin("ota_auth", true));
    TEST_ASSERT_TRUE(persisted.getBool("set", false));
    TEST_ASSERT_EQUAL_UINT8(OtaCredentialCore::RECORD_VERSION, persisted.getUChar("version", 0));
    TEST_ASSERT_EQUAL_UINT32(OtaCredentialCore::PBKDF2_ITERATIONS, persisted.getUInt("iterations", 0));
    TEST_ASSERT_EQUAL_UINT(OtaCredentialCore::SALT_SIZE, persisted.getBytesLength("salt"));
    TEST_ASSERT_EQUAL_UINT(OtaCredentialCore::VERIFIER_SIZE, persisted.getBytesLength("verifier"));
    persisted.end();

    TEST_ASSERT_TRUE(OtaCredentialService::clearPassword());
    TEST_ASSERT_FALSE(OtaCredentialService::isProtected());
    TEST_ASSERT_FALSE(OtaCredentialService::verify(String("route-password")));
}

void test_reboot_scheduler_admits_once_and_executes_after_grace_period(void) {
    RebootSchedulerCore::Scheduler scheduler;
    RebootSchedulerFake fake;
    const RebootSchedulerCore::Operations operations = fakeRebootOperations(fake);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(RebootSchedulerCore::ScheduleResult::Accepted),
        static_cast<int>(scheduler.schedule(operations)));
    TEST_ASSERT_TRUE(scheduler.isPending());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(RebootSchedulerCore::ScheduleResult::Pending),
        static_cast<int>(scheduler.schedule(operations)));
    TEST_ASSERT_EQUAL_INT(1, fake.scheduleCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.waitCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.restartCalls);
    TEST_ASSERT_NOT_NULL(fake.task);
    TEST_ASSERT_NOT_NULL(fake.taskContext);

    fake.task(fake.taskContext);

    TEST_ASSERT_EQUAL_INT(1, fake.waitCalls);
    TEST_ASSERT_EQUAL_UINT32(1000U, fake.waitedMs);
    TEST_ASSERT_EQUAL_INT(1, fake.restartCalls);
}

void test_reboot_scheduler_rolls_back_failed_task_admission(void) {
    RebootSchedulerCore::Scheduler scheduler;
    RebootSchedulerFake fake;
    fake.scheduleResult = false;

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(RebootSchedulerCore::ScheduleResult::Unavailable),
        static_cast<int>(scheduler.schedule(fakeRebootOperations(fake))));
    TEST_ASSERT_FALSE(scheduler.isPending());
    TEST_ASSERT_EQUAL_INT(1, fake.scheduleCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.waitCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.restartCalls);

    fake.scheduleResult = true;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(RebootSchedulerCore::ScheduleResult::Accepted),
        static_cast<int>(scheduler.schedule(fakeRebootOperations(fake))));
    TEST_ASSERT_TRUE(scheduler.isPending());
    TEST_ASSERT_EQUAL_INT(2, fake.scheduleCalls);
}

struct TestNetworkConfiguration {
    std::string ip;
};

using TestProvisioningCore = ProvisioningAttemptCore::Controller<std::string, TestNetworkConfiguration>;

TestProvisioningCore::CandidateValue provisioningCandidate(const char *ssid, const bool save,
                                                           const char *ip = "") {
    TestProvisioningCore::CandidateValue candidate;
    candidate.ssid = ssid;
    candidate.password = "test-password";
    candidate.networkConfiguration.ip = ip;
    candidate.save = save;
    return candidate;
}

void test_provisioning_attempt_saved_and_temporary_success_publish_coherent_status(void) {
    TestProvisioningCore core;
    TestProvisioningCore::CandidateValue claimed;

    const auto savedToken = core.start(provisioningCandidate("saved-network", true, "192.168.1.50"));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProvisioningAttemptCore::GotIpResult::PersistenceRequired),
        static_cast<int>(core.gotIp(savedToken, "192.168.1.50", claimed)));
    TEST_ASSERT_EQUAL_STRING("saved-network", claimed.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("test-password", claimed.password.c_str());
    TEST_ASSERT_TRUE(core.completePersistence(
        savedToken, ProvisioningAttemptCore::PersistenceResult::Succeeded));
    auto snapshot = core.snapshot();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningAttemptCore::State::Connected),
                          static_cast<int>(snapshot.state));
    TEST_ASSERT_TRUE(snapshot.hasIp);
    TEST_ASSERT_TRUE(snapshot.provisioningReady);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", snapshot.ip.c_str());
    TEST_ASSERT_TRUE(snapshot.reason.empty());

    const auto temporaryToken = core.start(provisioningCandidate("temporary-network", false));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProvisioningAttemptCore::GotIpResult::Temporary),
        static_cast<int>(core.gotIp(temporaryToken, "10.0.0.20", claimed)));
    snapshot = core.snapshot();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningAttemptCore::State::Connected),
                          static_cast<int>(snapshot.state));
    TEST_ASSERT_TRUE(snapshot.hasIp);
    TEST_ASSERT_FALSE(snapshot.provisioningReady);
    TEST_ASSERT_EQUAL_STRING("PERSISTENCE_REQUIRED", snapshot.reason.c_str());
}

void test_provisioning_attempt_persistence_failures_never_become_ready(void) {
    TestProvisioningCore core;
    TestProvisioningCore::CandidateValue claimed;

    auto token = core.start(provisioningCandidate("credential-failure", true));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProvisioningAttemptCore::GotIpResult::PersistenceRequired),
        static_cast<int>(core.gotIp(token, "10.0.0.21", claimed)));
    TEST_ASSERT_TRUE(core.completePersistence(
        token, ProvisioningAttemptCore::PersistenceResult::CredentialFailed));
    auto snapshot = core.snapshot();
    TEST_ASSERT_FALSE(snapshot.provisioningReady);
    TEST_ASSERT_EQUAL_STRING("CREDENTIAL_PERSIST_FAILED", snapshot.reason.c_str());

    token = core.start(provisioningCandidate("network-failure", true));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProvisioningAttemptCore::GotIpResult::PersistenceRequired),
        static_cast<int>(core.gotIp(token, "10.0.0.22", claimed)));
    TEST_ASSERT_TRUE(core.completePersistence(
        token, ProvisioningAttemptCore::PersistenceResult::NetworkConfigurationFailed));
    snapshot = core.snapshot();
    TEST_ASSERT_FALSE(snapshot.provisioningReady);
    TEST_ASSERT_EQUAL_STRING("NETWORK_CONFIG_PERSIST_FAILED", snapshot.reason.c_str());
}

void test_provisioning_attempt_tokens_ignore_stale_and_out_of_order_events(void) {
    TestProvisioningCore core;
    TestProvisioningCore::CandidateValue claimed;
    const auto oldToken = core.start(provisioningCandidate("old-network", true));
    TEST_ASSERT_TRUE(core.fail(oldToken, "TIMEOUT"));
    const auto currentToken = core.start(provisioningCandidate("current-network", true));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningAttemptCore::GotIpResult::Stale),
                          static_cast<int>(core.gotIp(oldToken, "10.0.0.90", claimed)));
    TEST_ASSERT_FALSE(core.disconnect(oldToken, "DISCONNECTED"));
    TEST_ASSERT_FALSE(core.completePersistence(
        oldToken, ProvisioningAttemptCore::PersistenceResult::Succeeded));
    auto snapshot = core.snapshot();
    TEST_ASSERT_EQUAL_STRING("current-network", snapshot.ssid.c_str());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningAttemptCore::State::Connecting),
                          static_cast<int>(snapshot.state));
    TEST_ASSERT_FALSE(snapshot.hasIp);
    TEST_ASSERT_FALSE(snapshot.provisioningReady);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProvisioningAttemptCore::GotIpResult::PersistenceRequired),
        static_cast<int>(core.gotIp(currentToken, "10.0.0.23", claimed)));
    TEST_ASSERT_TRUE(core.completePersistence(
        currentToken, ProvisioningAttemptCore::PersistenceResult::Succeeded));
    snapshot = core.snapshot();
    TEST_ASSERT_TRUE(snapshot.provisioningReady);
    TEST_ASSERT_EQUAL_STRING("current-network", snapshot.ssid.c_str());
}

void test_provisioning_event_barrier_drains_queued_old_events_before_replacement(void) {
    using Event = ProvisioningEventGate::Event;
    using Action = ProvisioningEventGate::Action;
    ProvisioningEventGate::Gate<uint32_t> gate;

    gate.awaitStationStop(41U);
    auto decision = gate.onEvent(Event::StationStopped);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::StartConnection), static_cast<int>(decision.action));
    TEST_ASSERT_EQUAL_UINT32(41U, decision.token);

    // This old GOT_IP is queued while attempt 41 is active. Replacement 42
    // disarms delivery before that queued event reaches the stable callback.
    std::vector<Event> eventQueue{Event::StationGotIp};
    gate.awaitStationStop(42U);
    decision = gate.onEvent(eventQueue.front());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::Ignore), static_cast<int>(decision.action));
    TEST_ASSERT_EQUAL_UINT32(0U, decision.token);

    decision = gate.onEvent(Event::StationDisconnected);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::Ignore), static_cast<int>(decision.action));
    decision = gate.onEvent(Event::StationStopped);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::StartConnection), static_cast<int>(decision.action));
    TEST_ASSERT_EQUAL_UINT32(42U, decision.token);
    decision = gate.onEvent(Event::StationGotIp);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::Deliver), static_cast<int>(decision.action));
    TEST_ASSERT_EQUAL_UINT32(42U, decision.token);
}

void test_provisioning_attempt_rejects_zero_and_malformed_station_addresses(void) {
    TestProvisioningCore core;
    TestProvisioningCore::CandidateValue claimed;
    const auto token = core.start(provisioningCandidate("address-validation", true));
    const char *invalidAddresses[] = {
        "", "0.0.0.0", "192.168.1", "192.168.1.1.2", "192.168.1.256",
        "192.168.01.2", "192.168.-1.2", "not-an-address",
    };
    for (const char *address : invalidAddresses) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningAttemptCore::GotIpResult::Stale),
                              static_cast<int>(core.gotIp(token, address, claimed)));
        const auto snapshot = core.snapshot();
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningAttemptCore::State::Connecting),
                              static_cast<int>(snapshot.state));
        TEST_ASSERT_FALSE(snapshot.hasIp);
        TEST_ASSERT_FALSE(snapshot.provisioningReady);
        TEST_ASSERT_TRUE(snapshot.ip.empty());
    }

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningAttemptCore::GotIpResult::PersistenceRequired),
                          static_cast<int>(core.gotIp(token, "192.168.1.2", claimed)));
}

void test_wifi_credential_fields_accept_full_width_ssid_and_password(void) {
    const std::string ssid(32U, 's');
    const std::string password(64U, 'p');
    uint8_t ssidField[32]{};
    uint8_t passwordField[64]{};

    TEST_ASSERT_TRUE(WifiCredentialFieldCore::copy(
        ssidField, sizeof(ssidField), ssid.c_str(), ssid.length()));
    TEST_ASSERT_TRUE(WifiCredentialFieldCore::copy(
        passwordField, sizeof(passwordField), password.c_str(), password.length()));
    TEST_ASSERT_TRUE(WifiCredentialFieldCore::equals(
        ssidField, sizeof(ssidField), ssid.c_str(), ssid.length()));
    TEST_ASSERT_TRUE(WifiCredentialFieldCore::equals(
        passwordField, sizeof(passwordField), password.c_str(), password.length()));

    const std::string oversizedSsid(33U, 's');
    const std::string oversizedPassword(65U, 'p');
    TEST_ASSERT_FALSE(WifiCredentialFieldCore::copy(
        ssidField, sizeof(ssidField), oversizedSsid.c_str(), oversizedSsid.length()));
    TEST_ASSERT_FALSE(WifiCredentialFieldCore::copy(
        passwordField, sizeof(passwordField), oversizedPassword.c_str(), oversizedPassword.length()));

    passwordField[63] = 'x';
    TEST_ASSERT_FALSE(WifiCredentialFieldCore::equals(
        passwordField, sizeof(passwordField), password.c_str(), password.length()));
}

void test_provisioning_attempt_cancel_and_failure_can_be_followed_by_success(void) {
    TestProvisioningCore core;
    TestProvisioningCore::CandidateValue claimed;
    auto token = core.start(provisioningCandidate("cancelled-network", true));
    TEST_ASSERT_TRUE(core.cancel(token));
    auto snapshot = core.snapshot();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningAttemptCore::State::Failed),
                          static_cast<int>(snapshot.state));
    TEST_ASSERT_EQUAL_STRING("CANCELLED", snapshot.reason.c_str());

    token = core.start(provisioningCandidate("failed-network", true));
    TEST_ASSERT_TRUE(core.disconnect(token, "WRONG_PASSWORD"));
    snapshot = core.snapshot();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningAttemptCore::State::Failed),
                          static_cast<int>(snapshot.state));

    token = core.start(provisioningCandidate("working-network", true));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProvisioningAttemptCore::GotIpResult::PersistenceRequired),
        static_cast<int>(core.gotIp(token, "10.0.0.24", claimed)));
    TEST_ASSERT_TRUE(core.completePersistence(
        token, ProvisioningAttemptCore::PersistenceResult::Succeeded));
    snapshot = core.snapshot();
    TEST_ASSERT_TRUE(snapshot.provisioningReady);
    TEST_ASSERT_EQUAL_STRING("working-network", snapshot.ssid.c_str());
}

void test_wifi_configuration_storage_round_trips_static_dhcp_and_clear(void) {
    Preferences::resetTestStorage();
    WifiStaticConfig saved;
    saved.ip = "192.168.20.72";
    saved.gateway = "192.168.20.1";
    saved.subnet = "255.255.255.0";
    saved.dns1 = "1.1.1.1";
    saved.dns2 = "8.8.8.8";
    TEST_ASSERT_TRUE(WifiConfigurationStorage::save(saved));

    WifiStaticConfig loaded;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiConfigurationStorage::LoadResult::Static),
                          static_cast<int>(WifiConfigurationStorage::load(loaded)));
    TEST_ASSERT_EQUAL_STRING(saved.ip.c_str(), loaded.ip.c_str());
    TEST_ASSERT_EQUAL_STRING(saved.gateway.c_str(), loaded.gateway.c_str());
    TEST_ASSERT_EQUAL_STRING(saved.subnet.c_str(), loaded.subnet.c_str());
    TEST_ASSERT_EQUAL_STRING(saved.dns1.c_str(), loaded.dns1.c_str());
    TEST_ASSERT_EQUAL_STRING(saved.dns2.c_str(), loaded.dns2.c_str());

    TEST_ASSERT_TRUE(WifiConfigurationStorage::save({}));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiConfigurationStorage::LoadResult::Dhcp),
                          static_cast<int>(WifiConfigurationStorage::load(loaded)));
    TEST_ASSERT_FALSE(loaded.any());

    TEST_ASSERT_TRUE(WifiConfigurationStorage::clear());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiConfigurationStorage::LoadResult::NotFound),
                          static_cast<int>(WifiConfigurationStorage::load(loaded)));
}

void test_wifi_configuration_storage_reports_write_clear_and_open_failures(void) {
    Preferences::resetTestStorage();
    WifiStaticConfig configuration;
    configuration.ip = "192.168.20.72";
    configuration.gateway = "192.168.20.1";
    configuration.subnet = "255.255.255.0";

    Preferences::setWriteFailure(true);
    TEST_ASSERT_FALSE(WifiConfigurationStorage::save(configuration));
    Preferences::setWriteFailure(false);
    TEST_ASSERT_TRUE(WifiConfigurationStorage::save(configuration));

    Preferences::setClearFailure(true);
    TEST_ASSERT_FALSE(WifiConfigurationStorage::clear());
    Preferences::setClearFailure(false);

    WifiStaticConfig loaded;
    Preferences::setBeginFailure(true);
    TEST_ASSERT_FALSE(WifiConfigurationStorage::save(configuration));
    TEST_ASSERT_FALSE(WifiConfigurationStorage::clear());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiConfigurationStorage::LoadResult::Unavailable),
                          static_cast<int>(WifiConfigurationStorage::load(loaded)));
    Preferences::setBeginFailure(false);
}

struct ProvisioningCompletionFake {
    ProvisioningCompletionCore::ScheduleResult result = ProvisioningCompletionCore::ScheduleResult::Accepted;
    int calls = 0;
};

ProvisioningCompletionCore::ScheduleResult scheduleProvisioningCompletion(void *context) {
    auto *fake = static_cast<ProvisioningCompletionFake *>(context);
    ++fake->calls;
    return fake->result;
}

void test_provisioning_completion_requires_one_fully_ready_snapshot(void) {
    ProvisioningCompletionFake fake;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningCompletionCore::Result::NotReady),
                          static_cast<int>(ProvisioningCompletionCore::complete(
                              false, std::string("192.168.20.72"), true,
                              scheduleProvisioningCompletion, &fake)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningCompletionCore::Result::NotReady),
                          static_cast<int>(ProvisioningCompletionCore::complete(
                              true, std::string(""), true,
                              scheduleProvisioningCompletion, &fake)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningCompletionCore::Result::NotReady),
                          static_cast<int>(ProvisioningCompletionCore::complete(
                              true, std::string("192.168.20.72"), false,
                              scheduleProvisioningCompletion, &fake)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningCompletionCore::Result::NotReady),
                          static_cast<int>(ProvisioningCompletionCore::complete(
                              true, std::string("0.0.0.0"), true,
                              scheduleProvisioningCompletion, &fake)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningCompletionCore::Result::NotReady),
                          static_cast<int>(ProvisioningCompletionCore::complete(
                              true, std::string("192.168.20.999"), true,
                              scheduleProvisioningCompletion, &fake)));
    TEST_ASSERT_EQUAL_INT(0, fake.calls);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningCompletionCore::Result::Accepted),
                          static_cast<int>(ProvisioningCompletionCore::complete(
                              true, std::string("192.168.20.72"), true,
                              scheduleProvisioningCompletion, &fake)));
    TEST_ASSERT_EQUAL_INT(1, fake.calls);
}

void test_provisioning_completion_maps_pending_and_unavailable_without_retrying(void) {
    ProvisioningCompletionFake fake;
    fake.result = ProvisioningCompletionCore::ScheduleResult::Pending;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningCompletionCore::Result::Pending),
                          static_cast<int>(ProvisioningCompletionCore::complete(
                              true, std::string("192.168.20.72"), true,
                              scheduleProvisioningCompletion, &fake)));
    TEST_ASSERT_EQUAL_INT(1, fake.calls);

    fake.result = ProvisioningCompletionCore::ScheduleResult::Unavailable;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningCompletionCore::Result::Unavailable),
                          static_cast<int>(ProvisioningCompletionCore::complete(
                              true, std::string("192.168.20.72"), true,
                              scheduleProvisioningCompletion, &fake)));
    TEST_ASSERT_EQUAL_INT(2, fake.calls);
    TEST_ASSERT_EQUAL_STRING(R"({"ok":false,"error":"provisioning_not_ready"})",
                             ProvisioningCompletionCore::NOT_READY_BODY);
    TEST_ASSERT_EQUAL_STRING(R"({"ok":false,"error":"reboot_pending"})",
                             ProvisioningCompletionCore::PENDING_BODY);
    TEST_ASSERT_EQUAL_STRING(R"({"ok":false,"error":"reboot_unavailable"})",
                             ProvisioningCompletionCore::UNAVAILABLE_BODY);
    const std::string accepted = ProvisioningCompletionCore::acceptedBody(std::string("192.168.20.72"));
    TEST_ASSERT_EQUAL_STRING(R"({"ok":true,"rebooting":true,"ip":"192.168.20.72"})", accepted.c_str());
}

void test_factory_reset_reports_success_only_after_all_steps_succeed(void) {
    FactoryResetFake fake;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(FactoryResetCore::StartResult::Accepted),
                          static_cast<int>(FactoryResetCore::start(fakeScheduleResetTask, &fake)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FactoryResetCore::RunResult::Ok),
                          static_cast<int>(executeFakeFactoryReset(fake)));
    TEST_ASSERT_EQUAL_INT(1, fake.scheduleCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.formatCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.eraseCalls);
}

void test_factory_reset_task_creation_failure_prevents_erasure(void) {
    FactoryResetFake fake;
    fake.scheduleResult = false;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(FactoryResetCore::StartResult::TaskUnavailable),
                          static_cast<int>(FactoryResetCore::start(fakeScheduleResetTask, &fake)));
    TEST_ASSERT_EQUAL_INT(1, fake.scheduleCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.formatCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.eraseCalls);
}

void test_factory_reset_config_format_failure_is_reported(void) {
    FactoryResetFake fake;
    fake.formatResult = false;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(FactoryResetCore::RunResult::ConfigStorageFailed),
                          static_cast<int>(executeFakeFactoryReset(fake)));
    TEST_ASSERT_EQUAL_INT(1, fake.formatCalls);
    TEST_ASSERT_EQUAL_INT(0, fake.eraseCalls);
}

void test_factory_reset_nvs_failure_is_reported(void) {
    FactoryResetFake fake;
    fake.eraseResult = false;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(FactoryResetCore::RunResult::NvsStorageFailed),
                          static_cast<int>(executeFakeFactoryReset(fake)));
    TEST_ASSERT_EQUAL_INT(1, fake.formatCalls);
    TEST_ASSERT_EQUAL_INT(1, fake.eraseCalls);
}

void test_registered_ota_routes_decode_bearer_and_dispatch_first_upload_chunk(void) {
    constexpr const char *correctBearer = "cm91dGUtcGFzc3dvcmQ=";
    const char *uploadRoutes[] = {Routes::OTA_FIRMWARE, Routes::OTA_FILESYSTEM};

    for (int routeMode = 0; routeMode < 2; ++routeMode) {
        Preferences::resetTestStorage();
        RouteTestState state;
        state.mode = routeMode;
        RouteTestServer server = configuredRouteTestServer(state);

        TEST_ASSERT_EQUAL_UINT(3U, server.count(Routes::OTA_FIRMWARE, ROUTE_TEST_POST));
        TEST_ASSERT_EQUAL_UINT(3U, server.count(Routes::OTA_FILESYSTEM, ROUTE_TEST_POST));
        TEST_ASSERT_EQUAL_UINT(4U, server.count(Routes::OTA_HTTP_CHECK, ROUTE_TEST_POST));
        TEST_ASSERT_EQUAL_UINT(4U, server.count(Routes::FACTORY_RESET, ROUTE_TEST_POST));

        const std::string initialSession = createRouteSession(server, state);
        TEST_ASSERT_FALSE(initialSession.empty());

        RouteTestRequest setPassword{ROUTE_TEST_PUT, Routes::OTA_PASSWORD, "", {}, &state};
        setPassword.session = initialSession;
        TEST_ASSERT_TRUE(server.dispatchBody(setPassword, R"({"password":"route-password"})"));
        TEST_ASSERT_EQUAL_INT(1, state.passwordHandlerCalls);
        TEST_ASSERT_TRUE(OtaCredentialService::isProtected());
        TEST_ASSERT_TRUE(state.sessionToken.empty());

        const std::string session = createRouteSession(server, state, correctBearer);
        TEST_ASSERT_FALSE(session.empty());
        TEST_ASSERT_EQUAL_INT(1, state.passwordVerificationCalls);

        RouteTestRequest absentCheck{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, "", {}, &state};
        TEST_ASSERT_TRUE(server.dispatchRequest(absentCheck));
        TEST_ASSERT_EQUAL_INT(1, state.unauthorizedCalls);
        TEST_ASSERT_EQUAL_INT(0, state.checkCalls);

        RouteTestRequest malformedCheck{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, "", {}, &state};
        malformedCheck.session = "%%%not-a-session%%%";
        TEST_ASSERT_TRUE(server.dispatchRequest(malformedCheck));
        TEST_ASSERT_EQUAL_INT(2, state.unauthorizedCalls);
        TEST_ASSERT_EQUAL_INT(0, state.checkCalls);

        RouteTestRequest wrongCheck{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, "", {}, &state};
        wrongCheck.session = "wrong-session";
        TEST_ASSERT_TRUE(server.dispatchRequest(wrongCheck));
        TEST_ASSERT_EQUAL_INT(3, state.unauthorizedCalls);
        TEST_ASSERT_EQUAL_INT(0, state.checkCalls);

        RouteTestRequest correctCheck{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, "", {}, &state};
        correctCheck.session = session;
        TEST_ASSERT_TRUE(server.dispatchRequest(correctCheck));
        TEST_ASSERT_EQUAL_INT(1, state.checkCalls);

        RouteTestRequest correctNotes{ROUTE_TEST_POST, Routes::OTA_HTTP_NOTES, "", {}, &state};
        RouteTestRequest correctApply{ROUTE_TEST_POST, Routes::OTA_HTTP_APPLY, "", {}, &state};
        correctNotes.session = session;
        correctApply.session = session;
        TEST_ASSERT_TRUE(server.dispatchRequest(correctNotes));
        TEST_ASSERT_TRUE(server.dispatchRequest(correctApply));
        TEST_ASSERT_EQUAL_INT(1, state.notesCalls);
        TEST_ASSERT_EQUAL_INT(1, state.applyCalls);
        TEST_ASSERT_EQUAL_INT(1, state.passwordVerificationCalls);

        for (int imageType = 0; imageType < 2; ++imageType) {
            const int beginCallsBefore = state.firmwareBeginCalls + state.filesystemBeginCalls;
            const int writesBefore = state.writeCalls;
            uint8_t firstChunk[] = {1U, 2U, 3U};

            RouteTestRequest absent{ROUTE_TEST_POST, uploadRoutes[imageType], "", {}, &state};
            TEST_ASSERT_TRUE(server.dispatchUploadChunk(absent, firstChunk, sizeof(firstChunk)));
            RouteTestRequest wrong{ROUTE_TEST_POST, uploadRoutes[imageType], "", {}, &state};
            wrong.session = "wrong-session";
            TEST_ASSERT_TRUE(server.dispatchUploadChunk(wrong, firstChunk, sizeof(firstChunk)));
            TEST_ASSERT_EQUAL_INT(beginCallsBefore, state.firmwareBeginCalls + state.filesystemBeginCalls);
            TEST_ASSERT_EQUAL_INT(writesBefore, state.writeCalls);

            RouteTestRequest correct{ROUTE_TEST_POST, uploadRoutes[imageType], "", {}, &state};
            correct.session = session;
            TEST_ASSERT_TRUE(server.dispatchUploadChunk(correct, firstChunk, sizeof(firstChunk)));
            TEST_ASSERT_EQUAL_INT(beginCallsBefore + 1, state.firmwareBeginCalls + state.filesystemBeginCalls);
            TEST_ASSERT_EQUAL_INT(writesBefore + 1, state.writeCalls);
        }
        TEST_ASSERT_EQUAL_INT(1, state.firmwareBeginCalls);
        TEST_ASSERT_EQUAL_INT(1, state.filesystemBeginCalls);
        TEST_ASSERT_EQUAL_INT(2, state.writeCalls);
        TEST_ASSERT_EQUAL_INT(routeMode, state.mode);
    }
}

void test_registered_factory_reset_route_clears_persisted_credential(void) {
    constexpr const char *correctBearer = "cm91dGUtcGFzc3dvcmQ=";
    Preferences::resetTestStorage();
    RouteTestState state;
    RouteTestServer server = configuredRouteTestServer(state);

    const std::string initialSession = createRouteSession(server, state);
    RouteTestRequest setPassword{ROUTE_TEST_PUT, Routes::OTA_PASSWORD, "", {}, &state};
    setPassword.session = initialSession;
    TEST_ASSERT_TRUE(server.dispatchBody(setPassword, R"({"password":"route-password"})"));
    TEST_ASSERT_TRUE(OtaCredentialService::isProtected());

    RouteTestRequest rejectedReset{ROUTE_TEST_POST, Routes::FACTORY_RESET, "", {}, &state};
    TEST_ASSERT_TRUE(server.dispatchBody(rejectedReset, R"({"confirm":"factory-reset"})"));
    TEST_ASSERT_EQUAL_INT(1, state.unauthorizedCalls);
    TEST_ASSERT_EQUAL_INT(0, state.factoryResetHandlerCalls);
    TEST_ASSERT_TRUE(OtaCredentialService::isProtected());

    const std::string session = createRouteSession(server, state, correctBearer);
    RouteTestRequest acceptedReset{ROUTE_TEST_POST, Routes::FACTORY_RESET, "", {}, &state};
    acceptedReset.session = session;
    TEST_ASSERT_TRUE(server.dispatchBody(acceptedReset, R"({"confirm":"factory-reset"})"));
    TEST_ASSERT_EQUAL_INT(1, state.factoryResetHandlerCalls);
    TEST_ASSERT_EQUAL_INT(1, state.factoryResetScheduleCalls);
    TEST_ASSERT_EQUAL_INT(1, state.factoryResetFormatCalls);
    TEST_ASSERT_EQUAL_INT(1, state.factoryResetEraseCalls);
    TEST_ASSERT_FALSE(OtaCredentialService::isProtected());
    TEST_ASSERT_FALSE(OtaCredentialService::verify(String("route-password")));
}

void test_registered_ota_routes_fail_closed_when_credential_storage_is_unavailable(void) {
    constexpr const char *correctBearer = "cm91dGUtcGFzc3dvcmQ=";
    const char *uploadRoutes[] = {Routes::OTA_FIRMWARE, Routes::OTA_FILESYSTEM};

    for (int routeMode = 0; routeMode < 2; ++routeMode) {
        Preferences::resetTestStorage();
        TEST_ASSERT_EQUAL_INT(static_cast<int>(OtaCredentialService::SaveResult::Ok),
                              static_cast<int>(OtaCredentialService::setPassword(String("route-password"))));

        RouteTestState state;
        state.mode = routeMode;
        RouteTestServer server = configuredRouteTestServer(state);
        Preferences::setBeginFailure(true);

        TEST_ASSERT_TRUE(createRouteSession(server, state, correctBearer).empty());

        RouteTestRequest check{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, correctBearer, {}, &state};
        RouteTestRequest notes{ROUTE_TEST_POST, Routes::OTA_HTTP_NOTES, correctBearer, {}, &state};
        RouteTestRequest apply{ROUTE_TEST_POST, Routes::OTA_HTTP_APPLY, correctBearer, {}, &state};
        TEST_ASSERT_TRUE(server.dispatchRequest(check));
        TEST_ASSERT_TRUE(server.dispatchRequest(notes));
        TEST_ASSERT_TRUE(server.dispatchRequest(apply));

        RouteTestRequest setPassword{ROUTE_TEST_PUT, Routes::OTA_PASSWORD, correctBearer, {}, &state};
        RouteTestRequest deletePassword{ROUTE_TEST_DELETE, Routes::OTA_PASSWORD, correctBearer, {}, &state};
        TEST_ASSERT_TRUE(server.dispatchBody(setPassword, R"({"password":"replacement-password"})"));
        TEST_ASSERT_TRUE(server.dispatchRequest(deletePassword));

        RouteTestRequest factoryReset{ROUTE_TEST_POST, Routes::FACTORY_RESET, correctBearer, {}, &state};
        TEST_ASSERT_TRUE(server.dispatchBody(factoryReset, R"({"confirm":"factory-reset"})"));

        for (int imageType = 0; imageType < 2; ++imageType) {
            uint8_t firstChunk[] = {1U, 2U, 3U};
            RouteTestRequest upload{ROUTE_TEST_POST, uploadRoutes[imageType], correctBearer, {}, &state};
            TEST_ASSERT_TRUE(server.dispatchUploadChunk(upload, firstChunk, sizeof(firstChunk)));
        }

        TEST_ASSERT_EQUAL_INT(9, state.unauthorizedCalls);
        TEST_ASSERT_EQUAL_INT(1, state.passwordVerificationCalls);
        TEST_ASSERT_EQUAL_INT(0, state.checkCalls);
        TEST_ASSERT_EQUAL_INT(0, state.notesCalls);
        TEST_ASSERT_EQUAL_INT(0, state.applyCalls);
        TEST_ASSERT_EQUAL_INT(0, state.passwordHandlerCalls);
        TEST_ASSERT_EQUAL_INT(0, state.deletePasswordCalls);
        TEST_ASSERT_EQUAL_INT(0, state.factoryResetHandlerCalls);
        TEST_ASSERT_EQUAL_INT(0, state.factoryResetScheduleCalls);
        TEST_ASSERT_EQUAL_INT(0, state.factoryResetFormatCalls);
        TEST_ASSERT_EQUAL_INT(0, state.factoryResetEraseCalls);
        TEST_ASSERT_EQUAL_INT(0, state.firmwareBeginCalls);
        TEST_ASSERT_EQUAL_INT(0, state.filesystemBeginCalls);
        TEST_ASSERT_EQUAL_INT(0, state.writeCalls);
        TEST_ASSERT_EQUAL_INT(routeMode, state.mode);
    }

    Preferences::setBeginFailure(false);
}

void test_mutation_request_guard_validates_marker_and_allowed_hosts(void) {
    TEST_ASSERT_TRUE(MutationRequestGuardCore::hasValidMarker("1"));
    TEST_ASSERT_FALSE(MutationRequestGuardCore::hasValidMarker(nullptr));
    TEST_ASSERT_FALSE(MutationRequestGuardCore::hasValidMarker(""));
    TEST_ASSERT_FALSE(MutationRequestGuardCore::hasValidMarker("01"));
    TEST_ASSERT_FALSE(MutationRequestGuardCore::hasValidMarker("1 "));

    const char *allowedHosts[] = {
        "192.168.1.42",
        "192.168.1.42:80",
        "192.168.4.1",
        "192.168.4.1:8080",
        "modbus-to-x",
        "MODBUS-TO-X:80",
        "modbus-to-x.local",
        "MODBUS-TO-X.LOCAL:8000",
    };
    for (const char *host : allowedHosts) {
        TEST_ASSERT_TRUE(MutationRequestGuardCore::isAllowed(
            "1", host, "192.168.1.42", "192.168.4.1", "modbus-to-x"));
    }

    const char *rejectedHosts[] = {
        "",
        "0.0.0.0",
        "192.168.1.43",
        "modbus-to-x.example",
        "evil-modbus-to-x.local",
        "modbus-to-x.local.",
        "modbus-to-x:0",
        "modbus-to-x:65536",
        "modbus-to-x:http",
        "modbus-to-x:80:90",
        "modbus-to-x,evil.example",
        "user@modbus-to-x",
        "[192.168.1.42]",
    };
    for (const char *host : rejectedHosts) {
        TEST_ASSERT_FALSE(MutationRequestGuardCore::isAllowed(
            "1", host, "192.168.1.42", "192.168.4.1", "modbus-to-x"));
    }
    TEST_ASSERT_FALSE(MutationRequestGuardCore::isAllowed(
        "1", "0.0.0.0", "0.0.0.0", "0.0.0.0", "modbus-to-x"));
    TEST_ASSERT_EQUAL_STRING(
        R"({"error":"forbidden_request_context"})", MutationRequestGuardCore::FORBIDDEN_RESPONSE);
}

void test_registered_mutation_matrix_fails_closed_before_dispatch(void) {
    struct MutationCase {
        const char *path;
        RouteTestMethod method;
        bool hasBody;
    };
    const MutationCase stationRoutes[] = {
        {Routes::PUT_MODBUS_CONFIG, ROUTE_TEST_PUT, true},
        {Routes::PUT_MQTT_CONFIG, ROUTE_TEST_PUT, true},
        {Routes::PUT_MQTT_SECRET, ROUTE_TEST_POST, true},
        {Routes::MQTT_TEST_CONNECT, ROUTE_TEST_POST, false},
        {Routes::POST_MODBUS_EXECUTE, ROUTE_TEST_POST, false},
        {Routes::POST_MBUS_DISABLE, ROUTE_TEST_POST, false},
        {Routes::POST_MBUS_ENABLE, ROUTE_TEST_POST, false},
        {Routes::DEVICE_RESET, ROUTE_TEST_POST, false},
        {Routes::POST_WIFI_RESET, ROUTE_TEST_POST, false},
    };
    const MutationCase accessPointRoutes[] = {
        {Routes::POST_WIFI_CONNECT, ROUTE_TEST_POST, true},
        {Routes::POST_WIFI_AP_OFF, ROUTE_TEST_POST, false},
        {Routes::POST_WIFI_CANCEL, ROUTE_TEST_POST, false},
        {Routes::POST_WIFI_RESET, ROUTE_TEST_POST, false},
        {Routes::DEVICE_RESET, ROUTE_TEST_POST, false},
    };

    for (int modeIndex = 0; modeIndex < 2; ++modeIndex) {
        RouteTestState state;
        const bool station = modeIndex == 0;
        RouteTestServer server = configuredMutationTestServer(
            state, station ? MutationRouteRegistration::Mode::Station
                           : MutationRouteRegistration::Mode::AccessPoint);
        const MutationCase *routes = station ? stationRoutes : accessPointRoutes;
        const size_t routeCount = station
            ? sizeof(stationRoutes) / sizeof(stationRoutes[0])
            : sizeof(accessPointRoutes) / sizeof(accessPointRoutes[0]);

        for (size_t i = 0U; i < routeCount; ++i) {
            const MutationCase &route = routes[i];
            TEST_ASSERT_EQUAL_UINT(3U, server.count(route.path, route.method));

            const int callsBefore = state.mutationSideEffectCalls;
            RouteTestRequest valid{route.method, route.path, "", {}, &state};
            TEST_ASSERT_TRUE(route.hasBody ? server.dispatchBody(valid, "{}") : server.dispatchRequest(valid));
            TEST_ASSERT_EQUAL_INT(callsBefore + 1, state.mutationSideEffectCalls);

            state.otaActive = true;
            RouteTestRequest duringOta{route.method, route.path, "", {}, &state};
            TEST_ASSERT_TRUE(route.hasBody
                                 ? server.dispatchBody(duringOta, R"({"mustNotAccumulate":true})")
                                 : server.dispatchRequest(duringOta));
            TEST_ASSERT_EQUAL_INT(callsBefore + 1, state.mutationSideEffectCalls);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(i + 1U), state.otaBusyCalls);
            state.otaActive = false;

            RouteTestRequest missingMarker{route.method, route.path, "", {}, &state};
            missingMarker.marker.clear();
            TEST_ASSERT_TRUE(route.hasBody
                                 ? server.dispatchBody(missingMarker, R"({"dangerous":true})")
                                 : server.dispatchRequest(missingMarker));
            TEST_ASSERT_EQUAL_INT(callsBefore + 1, state.mutationSideEffectCalls);

            RouteTestRequest wrongMarker{route.method, route.path, "", {}, &state};
            wrongMarker.marker = "0";
            TEST_ASSERT_TRUE(route.hasBody
                                 ? server.dispatchBody(wrongMarker, R"({"dangerous":true})")
                                 : server.dispatchRequest(wrongMarker));
            TEST_ASSERT_EQUAL_INT(callsBefore + 1, state.mutationSideEffectCalls);

            RouteTestRequest invalidHost{route.method, route.path, "", {}, &state};
            invalidHost.host = "attacker.example";
            TEST_ASSERT_TRUE(route.hasBody
                                 ? server.dispatchBody(invalidHost, R"({"dangerous":true})")
                                 : server.dispatchRequest(invalidHost));
            TEST_ASSERT_EQUAL_INT(callsBefore + 1, state.mutationSideEffectCalls);

            RouteTestRequest getRequest{ROUTE_TEST_GET, route.path, "", {}, &state};
            RouteTestRequest headRequest{ROUTE_TEST_HEAD, route.path, "", {}, &state};
            TEST_ASSERT_FALSE(server.dispatchRequest(getRequest));
            TEST_ASSERT_FALSE(server.dispatchRequest(headRequest));
            TEST_ASSERT_EQUAL_INT(callsBefore + 1, state.mutationSideEffectCalls);
        }
        TEST_ASSERT_EQUAL_INT(static_cast<int>(routeCount * 3U), state.forbiddenCalls);
        TEST_ASSERT_EQUAL_UINT(0U, server.count("/reset", ROUTE_TEST_GET));
        TEST_ASSERT_EQUAL_UINT(0U, server.count(Routes::POST_WIFI_RESET, ROUTE_TEST_GET));
        TEST_ASSERT_EQUAL_UINT(0U, server.count(Routes::POST_WIFI_RESET, ROUTE_TEST_HEAD));
        TEST_ASSERT_EQUAL_INT(1, state.rebootScheduleCalls);
        if (station) {
            TEST_ASSERT_EQUAL_UINT(0U, server.count(Routes::POST_WIFI_CONNECT, ROUTE_TEST_POST));
        } else {
            TEST_ASSERT_EQUAL_UINT(0U, server.count(Routes::PUT_MODBUS_CONFIG, ROUTE_TEST_PUT));
        }
    }
}

void test_registered_ota_matrix_checks_mutation_context_before_auth_or_payload(void) {
    Preferences::resetTestStorage();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(OtaCredentialService::SaveResult::Ok),
                          static_cast<int>(OtaCredentialService::setPassword(String("route-password"))));

    RouteTestState state;
    RouteTestServer server = configuredRouteTestServer(state);
    const char *requestRoutes[] = {
        Routes::OTA_HTTP_CHECK,
        Routes::OTA_HTTP_NOTES,
        Routes::OTA_HTTP_APPLY,
        Routes::OTA_PASSWORD,
    };
    const RouteTestMethod requestMethods[] = {
        ROUTE_TEST_POST,
        ROUTE_TEST_POST,
        ROUTE_TEST_POST,
        ROUTE_TEST_DELETE,
    };
    for (size_t i = 0U; i < sizeof(requestRoutes) / sizeof(requestRoutes[0]); ++i) {
        RouteTestRequest request{requestMethods[i], requestRoutes[i], "", {}, &state};
        request.marker.clear();
        TEST_ASSERT_TRUE(server.dispatchRequest(request));
    }

    RouteTestRequest settings{ROUTE_TEST_POST, Routes::OTA_HTTP_SETTINGS, "", {}, &state};
    settings.host = "public.example";
    TEST_ASSERT_TRUE(server.dispatchBody(settings, R"({"includePrereleases":true})"));

    RouteTestRequest setPassword{ROUTE_TEST_PUT, Routes::OTA_PASSWORD, "", {}, &state};
    setPassword.host = "public.example";
    TEST_ASSERT_TRUE(server.dispatchBody(setPassword, R"({"password":"replacement-password"})"));

    RouteTestRequest factoryReset{ROUTE_TEST_POST, Routes::FACTORY_RESET, "", {}, &state};
    factoryReset.marker = "invalid";
    TEST_ASSERT_TRUE(server.dispatchBody(factoryReset, R"({"confirm":"factory-reset"})"));

    uint8_t chunk[] = {1U, 2U, 3U};
    RouteTestRequest firmware{ROUTE_TEST_POST, Routes::OTA_FIRMWARE, "", {}, &state};
    firmware.host = "public.example";
    TEST_ASSERT_TRUE(server.dispatchUploadChunk(firmware, chunk, sizeof(chunk)));
    RouteTestRequest filesystem{ROUTE_TEST_POST, Routes::OTA_FILESYSTEM, "", {}, &state};
    filesystem.marker.clear();
    TEST_ASSERT_TRUE(server.dispatchUploadChunk(filesystem, chunk, sizeof(chunk)));

    TEST_ASSERT_EQUAL_INT(9, state.forbiddenCalls);
    TEST_ASSERT_EQUAL_INT(0, state.unauthorizedCalls);
    TEST_ASSERT_EQUAL_INT(0, state.checkCalls);
    TEST_ASSERT_EQUAL_INT(0, state.notesCalls);
    TEST_ASSERT_EQUAL_INT(0, state.applyCalls);
    TEST_ASSERT_EQUAL_INT(0, state.putSettingsCalls);
    TEST_ASSERT_EQUAL_INT(0, state.passwordHandlerCalls);
    TEST_ASSERT_EQUAL_INT(0, state.deletePasswordCalls);
    TEST_ASSERT_EQUAL_INT(0, state.factoryResetHandlerCalls);
    TEST_ASSERT_EQUAL_INT(0, state.firmwareBeginCalls);
    TEST_ASSERT_EQUAL_INT(0, state.filesystemBeginCalls);
    TEST_ASSERT_EQUAL_INT(0, state.writeCalls);

    RouteTestRequest unauthorized{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, "", {}, &state};
    TEST_ASSERT_TRUE(server.dispatchRequest(unauthorized));
    TEST_ASSERT_EQUAL_INT(1, state.unauthorizedCalls);
    TEST_ASSERT_EQUAL_INT(9, state.forbiddenCalls);

    constexpr const char *correctBearer = "cm91dGUtcGFzc3dvcmQ=";
    const std::string session = createRouteSession(server, state, correctBearer);
    RouteTestRequest accepted{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, "", {}, &state};
    accepted.session = session;
    accepted.host = "MODBUS-TO-X.LOCAL:80";
    TEST_ASSERT_TRUE(server.dispatchRequest(accepted));
    TEST_ASSERT_EQUAL_INT(1, state.checkCalls);

    RouteTestRequest readSettings{ROUTE_TEST_GET, Routes::OTA_HTTP_SETTINGS, "", {}, &state};
    readSettings.marker.clear();
    readSettings.host = "public.example";
    TEST_ASSERT_TRUE(server.dispatchRequest(readSettings));
    TEST_ASSERT_EQUAL_INT(1, state.getSettingsCalls);
}

void test_registered_ota_routes_apply_context_session_and_busy_precedence_before_body(void) {
    RouteTestState state;
    state.sessionToken = "active-session";
    state.otaActive = true;
    RouteTestServer server = configuredRouteTestServer(state);

    RouteTestRequest invalidContext{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, "", {}, &state};
    invalidContext.marker.clear();
    invalidContext.session = state.sessionToken;
    TEST_ASSERT_TRUE(server.dispatchRequest(invalidContext));
    TEST_ASSERT_EQUAL_INT(1, state.forbiddenCalls);
    TEST_ASSERT_EQUAL_INT(0, state.unauthorizedCalls);
    TEST_ASSERT_EQUAL_INT(0, state.otaBusyCalls);

    RouteTestRequest invalidSession{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, "", {}, &state};
    invalidSession.session = "stale-session";
    TEST_ASSERT_TRUE(server.dispatchRequest(invalidSession));
    TEST_ASSERT_EQUAL_INT(1, state.unauthorizedCalls);
    TEST_ASSERT_EQUAL_INT(0, state.otaBusyCalls);

    RouteTestRequest busyCheck{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, "", {}, &state};
    busyCheck.session = state.sessionToken;
    TEST_ASSERT_TRUE(server.dispatchRequest(busyCheck));
    TEST_ASSERT_EQUAL_INT(1, state.otaBusyCalls);
    TEST_ASSERT_EQUAL_INT(0, state.checkCalls);

    RouteTestRequest busySettings{ROUTE_TEST_POST, Routes::OTA_HTTP_SETTINGS, "", {}, &state};
    busySettings.session = state.sessionToken;
    TEST_ASSERT_TRUE(server.dispatchBody(busySettings, R"({"includePrereleases":true})"));
    TEST_ASSERT_EQUAL_INT(2, state.otaBusyCalls);
    TEST_ASSERT_EQUAL_INT(0, state.putSettingsCalls);

    RouteTestRequest concurrentApply{ROUTE_TEST_POST, Routes::OTA_HTTP_APPLY, "", {}, &state};
    concurrentApply.session = state.sessionToken;
    TEST_ASSERT_TRUE(server.dispatchRequest(concurrentApply));
    TEST_ASSERT_EQUAL_INT(3, state.otaBusyCalls);
    TEST_ASSERT_EQUAL_INT(0, state.applyCalls);

    RouteTestRequest manualStart{ROUTE_TEST_POST, Routes::OTA_MANUAL_START, "", {}, &state};
    manualStart.session = state.sessionToken;
    TEST_ASSERT_TRUE(server.dispatchBody(manualStart, R"({"component":"firmware","size":1024})"));
    TEST_ASSERT_EQUAL_INT(4, state.otaBusyCalls);
    TEST_ASSERT_EQUAL_INT(0, state.manualStartCalls);

    RouteTestRequest status{ROUTE_TEST_GET, Routes::OTA_STATUS, "", {}, &state};
    status.marker.clear();
    status.host = "public.example";
    TEST_ASSERT_TRUE(server.dispatchRequest(status));
    TEST_ASSERT_EQUAL_INT(1, state.statusCalls);
}

namespace {

std::string repeated(const std::string &value, const size_t count) {
    std::string result;
    result.reserve(value.size() * count);
    for (size_t i = 0U; i < count; ++i) result += value;
    return result;
}

MqttConfigCore::ConnectionInput validMqttInput() {
    MqttConfigCore::ConnectionInput input;
    input.brokerIp = "broker.local";
    input.brokerPort = "1883";
    return input;
}

struct MqttMutationFake {
    std::string config = "previous-config";
    std::string password = "previous-password";
    int configWrites = 0;
    int passwordWrites = 0;
    int reloads = 0;
    bool writeSucceeds = true;
    bool reloadSucceeds = true;
};

bool writeFakeMqttConfig(const char *value, const size_t length, void *context) {
    auto *fake = static_cast<MqttMutationFake *>(context);
    ++fake->configWrites;
    if (!fake->writeSucceeds) return false;
    fake->config.assign(value, length);
    return true;
}

bool writeFakeMqttPassword(const char *value, const size_t length, void *context) {
    auto *fake = static_cast<MqttMutationFake *>(context);
    ++fake->passwordWrites;
    if (!fake->writeSucceeds) return false;
    fake->password.assign(value, length);
    return true;
}

bool reloadFakeMqttConfig(void *context) {
    auto *fake = static_cast<MqttMutationFake *>(context);
    ++fake->reloads;
    return fake->reloadSucceeds;
}

}  // namespace

void test_mqtt_connection_field_byte_boundaries(void) {
    const size_t brokerLengths[] = {
        MqttConfigCore::BROKER_MAX_BYTES - 1U,
        MqttConfigCore::BROKER_MAX_BYTES,
        MqttConfigCore::BROKER_MAX_BYTES + 1U,
    };
    for (const size_t length : brokerLengths) {
        auto input = validMqttInput();
        input.brokerIp = repeated("b", length);
        MqttConfigCore::PreparedConnection connection;
        MqttConfigCore::ValidationError error;
        const bool expected = length <= MqttConfigCore::BROKER_MAX_BYTES;
        TEST_ASSERT_EQUAL(expected, MqttConfigCore::prepareConnection(input, connection, error));
        if (expected) {
            TEST_ASSERT_TRUE(connection.valid);
            TEST_ASSERT_EQUAL_UINT(length, std::strlen(connection.broker.data()));
            TEST_ASSERT_EQUAL_CHAR('\0', connection.broker[length]);
        } else {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigCore::Field::BrokerIp),
                                  static_cast<int>(error.field));
            TEST_ASSERT_FALSE(connection.valid);
        }
    }

    for (const size_t length : brokerLengths) {
        auto input = validMqttInput();
        input.brokerIp.clear();
        input.brokerUrl = "mqtt://" + repeated("h", length) + "/path-is-not-limited";
        MqttConfigCore::PreparedConnection connection;
        MqttConfigCore::ValidationError error;
        const bool expected = length <= MqttConfigCore::BROKER_MAX_BYTES;
        TEST_ASSERT_EQUAL(expected, MqttConfigCore::prepareConnection(input, connection, error));
        if (expected) {
            TEST_ASSERT_EQUAL_UINT(length, std::strlen(connection.broker.data()));
            TEST_ASSERT_EQUAL_CHAR('\0', connection.broker[length]);
        } else {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigCore::Field::BrokerUrlHost),
                                  static_cast<int>(error.field));
        }
    }

    const size_t credentialLengths[] = {
        MqttConfigCore::USER_MAX_BYTES - 1U,
        MqttConfigCore::USER_MAX_BYTES,
        MqttConfigCore::USER_MAX_BYTES + 1U,
    };
    for (const size_t length : credentialLengths) {
        auto input = validMqttInput();
        input.user = repeated("u", length);
        MqttConfigCore::PreparedConnection connection;
        MqttConfigCore::ValidationError error;
        const bool expected = length <= MqttConfigCore::USER_MAX_BYTES;
        TEST_ASSERT_EQUAL(expected, MqttConfigCore::prepareConnection(input, connection, error));
        if (expected) {
            TEST_ASSERT_EQUAL_UINT(length, std::strlen(connection.user.data()));
            TEST_ASSERT_EQUAL_CHAR('\0', connection.user[length]);
        } else {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigCore::Field::User),
                                  static_cast<int>(error.field));
        }
    }

    for (const size_t length : credentialLengths) {
        auto input = validMqttInput();
        input.password = repeated("p", length);
        MqttConfigCore::PreparedConnection connection;
        MqttConfigCore::ValidationError error;
        const bool expected = length <= MqttConfigCore::PASSWORD_MAX_BYTES;
        TEST_ASSERT_EQUAL(expected, MqttConfigCore::prepareConnection(input, connection, error));
        if (expected) {
            TEST_ASSERT_EQUAL_UINT(length, std::strlen(connection.password.data()));
            TEST_ASSERT_EQUAL_CHAR('\0', connection.password[length]);
        } else {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigCore::Field::Password),
                                  static_cast<int>(error.field));
        }
    }
}

void test_mqtt_multibyte_credentials_use_utf8_byte_lengths(void) {
    const std::string twoByteCharacter = "\xC3\xA9";
    const std::string thirtyBytes = repeated(twoByteCharacter, 15U);
    const std::string thirtyOneBytes = thirtyBytes + "a";
    const std::string thirtyTwoBytes = repeated(twoByteCharacter, 16U);
    const std::string values[] = {thirtyBytes, thirtyOneBytes, thirtyTwoBytes};

    for (size_t i = 0U; i < sizeof(values) / sizeof(values[0]); ++i) {
        auto input = validMqttInput();
        input.user = values[i];
        MqttConfigCore::PreparedConnection connection;
        MqttConfigCore::ValidationError error;
        const bool expected = values[i].size() <= MqttConfigCore::USER_MAX_BYTES;
        TEST_ASSERT_EQUAL(expected, MqttConfigCore::prepareConnection(input, connection, error));
        if (expected) {
            TEST_ASSERT_EQUAL_UINT(values[i].size(), std::strlen(connection.user.data()));
        } else {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigCore::Field::User),
                                  static_cast<int>(error.field));
        }
    }

    for (size_t i = 0U; i < sizeof(values) / sizeof(values[0]); ++i) {
        auto input = validMqttInput();
        input.password = values[i];
        MqttConfigCore::PreparedConnection connection;
        MqttConfigCore::ValidationError error;
        const bool expected = values[i].size() <= MqttConfigCore::PASSWORD_MAX_BYTES;
        TEST_ASSERT_EQUAL(expected, MqttConfigCore::prepareConnection(input, connection, error));
        if (expected) {
            TEST_ASSERT_EQUAL_UINT(values[i].size(), std::strlen(connection.password.data()));
        } else {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigCore::Field::Password),
                                  static_cast<int>(error.field));
        }
    }
}

void test_mqtt_port_contract(void) {
    struct PortCase {
        const char *text;
        bool valid;
        uint16_t value;
        MqttConfigCore::Constraint constraint;
    };
    const PortCase cases[] = {
        {"1", true, 1U, MqttConfigCore::Constraint::None},
        {"9999", true, 9999U, MqttConfigCore::Constraint::None},
        {"65534", true, 65534U, MqttConfigCore::Constraint::None},
        {"65535", true, 65535U, MqttConfigCore::Constraint::None},
        {"", false, 0U, MqttConfigCore::Constraint::PortDecimal},
        {" \t", false, 0U, MqttConfigCore::Constraint::PortDecimal},
        {"0", false, 0U, MqttConfigCore::Constraint::PortRange},
        {"65536", false, 0U, MqttConfigCore::Constraint::PortRange},
        {"18x3", false, 0U, MqttConfigCore::Constraint::PortDecimal},
        {"123456", false, 0U, MqttConfigCore::Constraint::MaximumBytes},
    };

    for (const auto &testCase : cases) {
        auto input = validMqttInput();
        input.brokerPort = testCase.text;
        MqttConfigCore::PreparedConnection connection;
        MqttConfigCore::ValidationError error;
        TEST_ASSERT_EQUAL(testCase.valid, MqttConfigCore::prepareConnection(input, connection, error));
        if (testCase.valid) TEST_ASSERT_EQUAL_UINT16(testCase.value, connection.port);
        else {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigCore::Field::BrokerPort),
                                  static_cast<int>(error.field));
            TEST_ASSERT_EQUAL_INT(static_cast<int>(testCase.constraint), static_cast<int>(error.constraint));
        }
    }
}

void test_mqtt_handler_core_rejects_before_storage_or_reload(void) {
    const std::string oversizedBroker = repeated("b", MqttConfigCore::BROKER_MAX_BYTES + 1U);
    const std::string oversizedUser = repeated("u", MqttConfigCore::USER_MAX_BYTES + 1U);
    struct InvalidConfigCase {
        std::string json;
        MqttConfigMutationCore::Status status;
        MqttConfigCore::Field field;
    };
    const InvalidConfigCase invalidConfigs[] = {
        {"{\"broker_ip\":\"" + oversizedBroker + "\",\"broker_port\":\"1883\"}",
         MqttConfigMutationCore::Status::InvalidField, MqttConfigCore::Field::BrokerIp},
        {"{\"broker_url\":\"mqtt://" + oversizedBroker + "/path\",\"broker_port\":\"1883\"}",
         MqttConfigMutationCore::Status::InvalidField, MqttConfigCore::Field::BrokerUrlHost},
        {"{\"broker_ip\":\"broker.local\",\"broker_port\":\"1883\",\"user\":\"" + oversizedUser + "\"}",
         MqttConfigMutationCore::Status::InvalidField, MqttConfigCore::Field::User},
        {R"({"broker_ip":"broker.local","broker_port":"65536"})",
         MqttConfigMutationCore::Status::InvalidField, MqttConfigCore::Field::BrokerPort},
        {R"({"broker_ip":"broker.local","broker_port":""})",
         MqttConfigMutationCore::Status::InvalidField, MqttConfigCore::Field::BrokerPort},
        {R"({"broker_ip":"broker.local","broker_port":"  "})",
         MqttConfigMutationCore::Status::InvalidField, MqttConfigCore::Field::BrokerPort},
        {"{not-json", MqttConfigMutationCore::Status::InvalidJson, MqttConfigCore::Field::None},
    };
    const MqttConfigMutationCore::ConfigOperations configOperations{
        writeFakeMqttConfig,
        reloadFakeMqttConfig,
    };

    for (const auto &testCase : invalidConfigs) {
        MqttMutationFake fake;
        const auto result = MqttConfigMutationCore::applyConfig(
            testCase.json.c_str(), testCase.json.size(), configOperations, &fake);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(testCase.status), static_cast<int>(result.status));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(testCase.field), static_cast<int>(result.validation.field));
        TEST_ASSERT_EQUAL_STRING("previous-config", fake.config.c_str());
        TEST_ASSERT_EQUAL_INT(0, fake.configWrites);
        TEST_ASSERT_EQUAL_INT(0, fake.reloads);
    }

    MqttMutationFake secretFake;
    const std::string oversizedPassword = repeated("p", MqttConfigCore::PASSWORD_MAX_BYTES + 1U);
    const std::string secretJson = "{\"password\":\"" + oversizedPassword + "\"}";
    const MqttConfigMutationCore::SecretOperations secretOperations{writeFakeMqttPassword};
    const auto secretResult = MqttConfigMutationCore::applySecret(
        secretJson.c_str(), secretJson.size(), secretOperations, &secretFake);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigMutationCore::Status::InvalidField),
                          static_cast<int>(secretResult.status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigCore::Field::Password),
                          static_cast<int>(secretResult.validation.field));
    TEST_ASSERT_EQUAL_STRING("previous-password", secretFake.password.c_str());
    TEST_ASSERT_EQUAL_INT(0, secretFake.passwordWrites);
}

void test_mqtt_handler_core_accepts_boundary_values(void) {
    MqttMutationFake fake;
    const std::string broker = repeated("b", MqttConfigCore::BROKER_MAX_BYTES);
    const std::string user = repeated("u", MqttConfigCore::USER_MAX_BYTES);
    const std::string configJson = "{\"enabled\":true,\"broker_ip\":\"" + broker
                                   + "\",\"broker_url\":\"\",\"broker_port\":\"65535\",\"user\":\""
                                   + user + "\",\"root_topic\":\"mbx_root\"}";
    const MqttConfigMutationCore::ConfigOperations configOperations{
        writeFakeMqttConfig,
        reloadFakeMqttConfig,
    };
    const auto configResult = MqttConfigMutationCore::applyConfig(
        configJson.c_str(), configJson.size(), configOperations, &fake);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigMutationCore::Status::Ok),
                          static_cast<int>(configResult.status));
    TEST_ASSERT_EQUAL_STRING(configJson.c_str(), fake.config.c_str());
    TEST_ASSERT_EQUAL_INT(1, fake.configWrites);
    TEST_ASSERT_EQUAL_INT(1, fake.reloads);

    const std::string password = repeated("p", MqttConfigCore::PASSWORD_MAX_BYTES);
    const std::string secretJson = "{\"password\":\"" + password + "\"}";
    const MqttConfigMutationCore::SecretOperations secretOperations{writeFakeMqttPassword};
    const auto secretResult = MqttConfigMutationCore::applySecret(
        secretJson.c_str(), secretJson.size(), secretOperations, &fake);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigMutationCore::Status::Ok),
                          static_cast<int>(secretResult.status));
    TEST_ASSERT_EQUAL_STRING(password.c_str(), fake.password.c_str());
    TEST_ASSERT_EQUAL_INT(1, fake.passwordWrites);
}

void test_mqtt_document_defaults_only_a_missing_port(void) {
    MqttConfigDocument::StoredConfig missingPort;
    constexpr const char *missingPortJson = R"({"broker_ip":"broker.local"})";
    TEST_ASSERT_TRUE(MqttConfigDocument::parseConfig(
        missingPortJson, std::strlen(missingPortJson), missingPort));
    TEST_ASSERT_EQUAL_STRING(MqttConfigCore::DEFAULT_PORT, missingPort.connection.brokerPort.c_str());

    MqttConfigDocument::StoredConfig emptyPort;
    constexpr const char *emptyPortJson = R"({"broker_ip":"broker.local","broker_port":""})";
    TEST_ASSERT_TRUE(MqttConfigDocument::parseConfig(
        emptyPortJson, std::strlen(emptyPortJson), emptyPort));
    TEST_ASSERT_TRUE(emptyPort.connection.brokerPort.empty());

    MqttConfigCore::PreparedConnection connection;
    MqttConfigCore::ValidationError error;
    TEST_ASSERT_FALSE(MqttConfigCore::prepareConnection(emptyPort.connection, connection, error));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigCore::Field::BrokerPort),
                          static_cast<int>(error.field));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(MqttConfigCore::Constraint::PortDecimal),
                          static_cast<int>(error.constraint));
}

void test_mqtt_one_shot_test_restores_enabled_state_after_valid_load(void) {
    struct TestCase {
        bool configuredEnabled;
        bool attemptResult;
    };
    const TestCase cases[] = {
        {true, true},
        {true, false},
        {false, true},
        {false, false},
    };

    for (const auto &testCase : cases) {
        bool runtimeEnabled = true;
        int loadCalls = 0;
        int attemptCalls = 0;
        int restoreCalls = 0;
        const bool connected = MqttConfigCore::runOneShotConnectionTest(
            [&](bool &configuredEnabled) {
                ++loadCalls;
                runtimeEnabled = false;
                configuredEnabled = testCase.configuredEnabled;
                return true;
            },
            [&]() {
                ++attemptCalls;
                return testCase.attemptResult;
            },
            [&](const bool configuredEnabled) {
                ++restoreCalls;
                runtimeEnabled = configuredEnabled;
            });

        TEST_ASSERT_EQUAL(testCase.attemptResult, connected);
        TEST_ASSERT_EQUAL(testCase.configuredEnabled, runtimeEnabled);
        TEST_ASSERT_EQUAL_INT(1, loadCalls);
        TEST_ASSERT_EQUAL_INT(1, attemptCalls);
        TEST_ASSERT_EQUAL_INT(1, restoreCalls);
    }

    bool runtimeEnabled = true;
    int attemptCalls = 0;
    int restoreCalls = 0;
    TEST_ASSERT_FALSE(MqttConfigCore::runOneShotConnectionTest(
        [&](bool &) {
            runtimeEnabled = false;
            return false;
        },
        [&]() {
            ++attemptCalls;
            return true;
        },
        [&](const bool configuredEnabled) {
            ++restoreCalls;
            runtimeEnabled = configuredEnabled;
        }));
    TEST_ASSERT_FALSE(runtimeEnabled);
    TEST_ASSERT_EQUAL_INT(0, attemptCalls);
    TEST_ASSERT_EQUAL_INT(0, restoreCalls);
}

void test_invalid_persisted_mqtt_fields_clear_state_and_gate_client_calls(void) {
    const std::string oversizedBroker = repeated("b", MqttConfigCore::BROKER_MAX_BYTES + 1U);
    const std::string oversizedUser = repeated("u", MqttConfigCore::USER_MAX_BYTES + 1U);
    struct PersistedCase {
        std::string json;
        std::string password;
    };
    const PersistedCase cases[] = {
        {"{\"broker_ip\":\"" + oversizedBroker + "\",\"broker_port\":\"1883\"}", ""},
        {"{\"broker_url\":\"mqtt://" + oversizedBroker + "\",\"broker_port\":\"1883\"}", ""},
        {"{\"broker_ip\":\"broker.local\",\"broker_port\":\"1883\",\"user\":\"" + oversizedUser + "\"}", ""},
        {R"({"broker_ip":"broker.local","broker_port":"123456"})", ""},
        {R"({"broker_ip":"broker.local","broker_port":""})", ""},
        {R"({"broker_ip":"broker.local","broker_port":"  "})", ""},
        {R"({"broker_ip":"broker.local","broker_port":"1883"})",
         repeated("p", MqttConfigCore::PASSWORD_MAX_BYTES + 1U)},
    };

    for (const auto &testCase : cases) {
        MqttConfigDocument::StoredConfig stored;
        TEST_ASSERT_TRUE(MqttConfigDocument::parseConfig(
            testCase.json.c_str(), testCase.json.size(), stored));
        stored.connection.password = testCase.password;

        MqttConfigCore::PreparedConnection connection;
        connection.valid = true;
        connection.broker[0] = 's';
        connection.user[0] = 's';
        connection.password[0] = 's';
        connection.port = 1883U;
        MqttConfigCore::ValidationError error;
        TEST_ASSERT_FALSE(MqttConfigCore::prepareConnection(stored.connection, connection, error));
        TEST_ASSERT_FALSE(connection.valid);
        TEST_ASSERT_EQUAL_CHAR('\0', connection.broker[0]);
        TEST_ASSERT_EQUAL_CHAR('\0', connection.user[0]);
        TEST_ASSERT_EQUAL_CHAR('\0', connection.password[0]);
        TEST_ASSERT_EQUAL_UINT16(0U, connection.port);

        int clientCalls = 0;
        TEST_ASSERT_FALSE(MqttConfigCore::withValidConnection(connection, [&clientCalls](const auto &) {
            ++clientCalls;
            return true;
        }));
        TEST_ASSERT_EQUAL_INT(0, clientCalls);
    }
}

struct RecordingMqttOwnerClient {
    std::vector<std::string> calls;
    bool connectResult = true;
    bool publishResult = true;

    void setServer(const std::string &server) { calls.push_back("server:" + server); }
    bool connect() {
        calls.push_back("connect");
        return connectResult;
    }
    void disconnect() { calls.push_back("disconnect"); }
    bool loop() {
        calls.push_back("loop");
        return true;
    }
    bool publish(const std::string &topic, const std::string &payload) {
        calls.push_back("publish:" + topic + "=" + payload);
        return publishResult;
    }
    void subscribe(const std::string &topic) { calls.push_back("subscribe:" + topic); }
    void unsubscribe(const std::string &topic) { calls.push_back("unsubscribe:" + topic); }
};

void test_owner_mailbox_is_bounded_fifo_and_owns_value_payloads(void) {
    struct Payload {
        int id = 0;
        std::string text;
    };
    OwnerMailboxCore<Payload, 2U> mailbox;
    Payload first{1, "first"};
    Payload second{2, "second"};
    TEST_ASSERT_TRUE(mailbox.tryPush(first));
    TEST_ASSERT_TRUE(mailbox.tryPush(second));
    TEST_ASSERT_FALSE(mailbox.tryPush(Payload{3, "overflow"}));
    first.text = "mutated-after-admission";

    Payload output;
    TEST_ASSERT_TRUE(mailbox.tryPop(output));
    TEST_ASSERT_EQUAL_INT(1, output.id);
    TEST_ASSERT_EQUAL_STRING("first", output.text.c_str());
    TEST_ASSERT_TRUE(mailbox.tryPop(output));
    TEST_ASSERT_EQUAL_INT(2, output.id);
    TEST_ASSERT_FALSE(mailbox.tryPop(output));
}

void test_owner_completion_is_exactly_once_and_cancels_only_before_start(void) {
    OwnerCompletionCore completed;
    TEST_ASSERT_TRUE(completed.markQueued());
    TEST_ASSERT_FALSE(completed.markQueued());
    TEST_ASSERT_TRUE(completed.tryStart());
    TEST_ASSERT_FALSE(completed.cancelIfQueued());
    TEST_ASSERT_TRUE(completed.complete());
    TEST_ASSERT_FALSE(completed.complete());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(OwnerCompletionCore::State::Completed),
                          static_cast<int>(completed.state()));

    OwnerCompletionCore cancelled;
    TEST_ASSERT_TRUE(cancelled.markQueued());
    TEST_ASSERT_TRUE(cancelled.cancelIfQueued());
    TEST_ASSERT_FALSE(cancelled.tryStart());
    TEST_ASSERT_FALSE(cancelled.complete());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(OwnerCompletionCore::State::Cancelled),
                          static_cast<int>(cancelled.state()));
}

void test_owner_mailbox_reports_overload_unavailable_shutdown_and_fifo_replacement(void) {
    using Mailbox = OwnerMailboxCore<int, 2U>;
    Mailbox mailbox;
    mailbox.setAvailable(false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mailbox::Admission::Unavailable),
                          static_cast<int>(mailbox.admit(1)));
    mailbox.setAvailable(true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mailbox::Admission::Accepted),
                          static_cast<int>(mailbox.admit(1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mailbox::Admission::Accepted),
                          static_cast<int>(mailbox.admit(2)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mailbox::Admission::Full),
                          static_cast<int>(mailbox.admit(3)));

    int appliedGeneration = 0;
    int candidate = 0;
    while (mailbox.tryPop(candidate)) appliedGeneration = candidate;
    TEST_ASSERT_EQUAL_INT(2, appliedGeneration);  // FIFO; the later complete config wins.

    mailbox.shutdown();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Mailbox::Admission::Shutdown),
                          static_cast<int>(mailbox.admit(4)));
}

void test_mqtt_owner_core_enforces_context_generation_and_client_order(void) {
    constexpr uintptr_t OWNER = 41U;
    RecordingMqttOwnerClient client;
    MqttOwnerCore<RecordingMqttOwnerClient> owner(client, OWNER);

    TEST_ASSERT_TRUE(owner.applyConfiguration(OWNER, "broker-a", true));
    std::vector<MqttOwnerCore<RecordingMqttOwnerClient>::Subscription> subscriptions;
    subscriptions.push_back({"root/write", [](const std::string &) {}});
    TEST_ASSERT_TRUE(owner.replaceSubscriptions(OWNER, subscriptions, 7U));
    TEST_ASSERT_TRUE(owner.connect(OWNER));
    TEST_ASSERT_TRUE(owner.loop(OWNER));
    std::vector<MqttOwnerCore<RecordingMqttOwnerClient>::Subscription> replacement;
    replacement.push_back({"root/new-write", [](const std::string &) {}});
    TEST_ASSERT_TRUE(owner.replaceSubscriptions(OWNER, replacement, 8U));
    TEST_ASSERT_TRUE(owner.applyConfiguration(OWNER, "broker-b", true));
    TEST_ASSERT_TRUE(owner.connect(OWNER));
    TEST_ASSERT_TRUE(owner.publish(OWNER, "root/state", "12", owner.generation(), 8U));
    TEST_ASSERT_FALSE(owner.publish(OWNER, "root/state", "stale", owner.generation() - 1U, 8U));
    TEST_ASSERT_FALSE(owner.publish(999U, "root/state", "wrong-context", owner.generation(), 7U));
    TEST_ASSERT_EQUAL_UINT32(1U, owner.ownerViolations());
    TEST_ASSERT_TRUE(owner.disable(OWNER));

    const char *expected[] = {
        "server:broker-a",
        "connect",
        "subscribe:root/write",
        "loop",
        "unsubscribe:root/write",
        "subscribe:root/new-write",
        "disconnect",
        "server:broker-b",
        "connect",
        "subscribe:root/new-write",
        "publish:root/state=12",
        "disconnect",
    };
    TEST_ASSERT_EQUAL_UINT(sizeof(expected) / sizeof(expected[0]), client.calls.size());
    for (size_t i = 0; i < client.calls.size(); ++i) {
        TEST_ASSERT_EQUAL_STRING(expected[i], client.calls[i].c_str());
    }
}

void test_mqtt_owner_queues_replacement_submitted_during_callback_dispatch(void) {
    constexpr uintptr_t OWNER = 12U;
    RecordingMqttOwnerClient client;
    MqttOwnerCore<RecordingMqttOwnerClient> owner(client, OWNER);
    OwnerMailboxCore<int, 1U> commands;
    int invocations = 0;
    std::vector<MqttOwnerCore<RecordingMqttOwnerClient>::Subscription> initial;
    initial.push_back({"old", [&](const std::string &) {
        ++invocations;
        TEST_ASSERT_TRUE(commands.tryPush(1));
    }});
    TEST_ASSERT_TRUE(owner.replaceSubscriptions(OWNER, std::move(initial), 1U));
    TEST_ASSERT_TRUE(owner.dispatch(OWNER, "old", "payload"));

    int command = 0;
    TEST_ASSERT_TRUE(commands.tryPop(command));
    std::vector<MqttOwnerCore<RecordingMqttOwnerClient>::Subscription> replacement;
    replacement.push_back({"new", [&](const std::string &) { ++invocations; }});
    TEST_ASSERT_TRUE(owner.replaceSubscriptions(OWNER, std::move(replacement), 2U));
    TEST_ASSERT_FALSE(owner.dispatch(OWNER, "old", "stale"));
    TEST_ASSERT_TRUE(owner.dispatch(OWNER, "new", "current"));
    TEST_ASSERT_EQUAL_INT(2, invocations);
}

void test_mqtt_owner_core_removal_during_dispatch_has_no_stale_invocation(void) {
    constexpr uintptr_t OWNER = 9U;
    RecordingMqttOwnerClient client;
    MqttOwnerCore<RecordingMqttOwnerClient> owner(client, OWNER);
    int oldCalls = 0;
    int newCalls = 0;

    std::vector<MqttOwnerCore<RecordingMqttOwnerClient>::Subscription> initial;
    initial.push_back({"old", [&](const std::string &) {
        ++oldCalls;
        std::vector<MqttOwnerCore<RecordingMqttOwnerClient>::Subscription> replacement;
        replacement.push_back({"new", [&](const std::string &) { ++newCalls; }});
        TEST_ASSERT_TRUE(owner.replaceSubscriptions(OWNER, std::move(replacement), 2U));
    }});
    TEST_ASSERT_TRUE(owner.replaceSubscriptions(OWNER, std::move(initial), 1U));
    TEST_ASSERT_TRUE(owner.dispatch(OWNER, "old", "first"));
    TEST_ASSERT_EQUAL_INT(1, oldCalls);
    TEST_ASSERT_FALSE(owner.dispatch(OWNER, "old", "stale"));
    TEST_ASSERT_TRUE(owner.dispatch(OWNER, "new", "current"));
    TEST_ASSERT_EQUAL_INT(1, newCalls);
}

void test_mqtt_owner_core_replacement_deduplicates_reconnect_subscriptions(void) {
    constexpr uintptr_t OWNER = 5U;
    RecordingMqttOwnerClient client;
    MqttOwnerCore<RecordingMqttOwnerClient> owner(client, OWNER);
    TEST_ASSERT_TRUE(owner.applyConfiguration(OWNER, "broker-a", true));
    std::vector<MqttOwnerCore<RecordingMqttOwnerClient>::Subscription> initial;
    initial.push_back({"same", [](const std::string &) {}});
    TEST_ASSERT_TRUE(owner.replaceSubscriptions(OWNER, initial, 1U));
    TEST_ASSERT_TRUE(owner.connect(OWNER));
    TEST_ASSERT_TRUE(owner.replaceSubscriptions(OWNER, initial, 1U));

    size_t subscribeCount = 0U;
    for (const auto &call : client.calls) {
        if (call == "subscribe:same") ++subscribeCount;
    }
    TEST_ASSERT_EQUAL_UINT(1U, subscribeCount);
}

void test_mqtt_owner_quiesces_after_retained_signal_and_rejects_work_until_matching_resume(void) {
    constexpr uintptr_t OWNER = 52U;
    RecordingMqttOwnerClient client;
    MqttOwnerCore<RecordingMqttOwnerClient> owner(client, OWNER);
    TEST_ASSERT_TRUE(owner.applyConfiguration(OWNER, "broker", true));
    TEST_ASSERT_TRUE(owner.connect(OWNER));
    TEST_ASSERT_TRUE(owner.quiesceForOta(OWNER, 91U, "root/status", "offline"));
    TEST_ASSERT_TRUE(owner.snapshot().quiesced);
    TEST_ASSERT_EQUAL_UINT32(91U, owner.snapshot().otaOperationId);
    TEST_ASSERT_FALSE(owner.loop(OWNER));
    TEST_ASSERT_FALSE(owner.publish(OWNER, "root/state", "late", owner.generation(), 0U));
    TEST_ASSERT_FALSE(owner.resumeFromOta(OWNER, 92U));
    TEST_ASSERT_TRUE(owner.resumeFromOta(OWNER, 91U));
    TEST_ASSERT_FALSE(owner.snapshot().quiesced);
    TEST_ASSERT_TRUE(owner.snapshot().enabled);
    TEST_ASSERT_TRUE(owner.connect(OWNER));

    const char *expected[] = {
        "server:broker",
        "connect",
        "publish:root/status=offline",
        "disconnect",
        "connect",
    };
    TEST_ASSERT_EQUAL_UINT(sizeof(expected) / sizeof(expected[0]), client.calls.size());
    for (size_t i = 0U; i < client.calls.size(); ++i) {
        TEST_ASSERT_EQUAL_STRING(expected[i], client.calls[i].c_str());
    }
}

void test_mqtt_owner_preserves_initially_disabled_state_across_ota(void) {
    constexpr uintptr_t OWNER = 53U;
    RecordingMqttOwnerClient client;
    MqttOwnerCore<RecordingMqttOwnerClient> owner(client, OWNER);
    TEST_ASSERT_TRUE(owner.applyConfiguration(OWNER, "broker", false));
    TEST_ASSERT_TRUE(owner.quiesceForOta(OWNER, 7U));
    TEST_ASSERT_TRUE(owner.resumeFromOta(OWNER, 7U));
    TEST_ASSERT_FALSE(owner.snapshot().enabled);
    TEST_ASSERT_FALSE(owner.connect(OWNER));
}

void test_mqtt_owner_quiesce_is_bounded_when_signal_fails_or_broker_is_disconnected(void) {
    constexpr uintptr_t OWNER = 54U;
    RecordingMqttOwnerClient failingClient;
    failingClient.publishResult = false;
    MqttOwnerCore<RecordingMqttOwnerClient> failingOwner(failingClient, OWNER);
    TEST_ASSERT_TRUE(failingOwner.applyConfiguration(OWNER, "broker", true));
    TEST_ASSERT_TRUE(failingOwner.connect(OWNER));
    TEST_ASSERT_TRUE(failingOwner.quiesceForOta(OWNER, 8U, "root/status", "offline"));
    TEST_ASSERT_EQUAL_STRING("publish:root/status=offline", failingClient.calls[2].c_str());
    TEST_ASSERT_EQUAL_STRING("disconnect", failingClient.calls[3].c_str());

    RecordingMqttOwnerClient disconnectedClient;
    MqttOwnerCore<RecordingMqttOwnerClient> disconnectedOwner(disconnectedClient, OWNER);
    TEST_ASSERT_TRUE(disconnectedOwner.applyConfiguration(OWNER, "broker", true));
    TEST_ASSERT_TRUE(disconnectedOwner.quiesceForOta(OWNER, 9U, "root/status", "offline"));
    TEST_ASSERT_EQUAL_UINT(1U, disconnectedClient.calls.size());
    TEST_ASSERT_EQUAL_STRING("server:broker", disconnectedClient.calls[0].c_str());
}

struct ModbusOwnerTestConfiguration {
    int identity = 0;
    int busValue = 0;
    size_t deviceCount = 0U;
    size_t datapointCount = 0U;
};

struct RecordingModbusOwnerBus {
    std::vector<int> initializedValues;
    void initialize(const int value) { initializedValues.push_back(value); }
};

void test_modbus_owner_activates_only_after_transaction_boundary(void) {
    RecordingModbusOwnerBus bus;
    ModbusOwnerCore<ModbusOwnerTestConfiguration, RecordingModbusOwnerBus> owner(bus);
    owner.requestActivation({1, 9600, 3U, 6U});
    TEST_ASSERT_TRUE(owner.processBoundary());
    owner.setActive(true);

    std::vector<int> observedIdentities;
    owner.runPollingPass([&](const ModbusOwnerTestConfiguration &active, const size_t deviceIndex) {
        observedIdentities.push_back(active.identity);
        if (deviceIndex == 0U) {
            owner.requestActivation({2, 19200, 2U, 4U});
            TEST_ASSERT_FALSE(owner.processBoundary());
        }
    });
    TEST_ASSERT_EQUAL_UINT(1U, observedIdentities.size());
    for (const int identity : observedIdentities) TEST_ASSERT_EQUAL_INT(1, identity);
    TEST_ASSERT_EQUAL_INT(1, owner.active().identity);
    TEST_ASSERT_TRUE(owner.processBoundary());
    TEST_ASSERT_EQUAL_INT(2, owner.active().identity);
    TEST_ASSERT_EQUAL_INT(19200, bus.initializedValues.back());
}

void test_modbus_owner_defers_activation_during_adhoc_command(void) {
    RecordingModbusOwnerBus bus;
    ModbusOwnerCore<ModbusOwnerTestConfiguration, RecordingModbusOwnerBus> owner(bus);
    owner.requestActivation({1, 9600, 1U, 1U});
    TEST_ASSERT_TRUE(owner.processBoundary());
    owner.runCommand([&](const ModbusOwnerTestConfiguration &active) {
        TEST_ASSERT_EQUAL_INT(1, active.identity);
        owner.requestActivation({2, 38400, 4U, 8U});
        TEST_ASSERT_FALSE(owner.processBoundary());
        TEST_ASSERT_EQUAL_INT(1, owner.active().identity);
    });
    TEST_ASSERT_TRUE(owner.processBoundary());
    TEST_ASSERT_EQUAL_INT(2, owner.active().identity);
}

void test_modbus_owner_snapshots_remain_values_across_replacement(void) {
    RecordingModbusOwnerBus bus;
    ModbusOwnerCore<ModbusOwnerTestConfiguration, RecordingModbusOwnerBus> owner(bus);
    owner.requestActivation({1, 9600, 2U, 10U});
    TEST_ASSERT_TRUE(owner.processBoundary());
    const auto oldSnapshot = owner.snapshot();

    for (int identity = 2; identity < 50; ++identity) {
        owner.requestActivation({identity, identity * 100, static_cast<size_t>(identity),
                                 static_cast<size_t>(identity * 3)});
        TEST_ASSERT_TRUE(owner.processBoundary());
    }
    TEST_ASSERT_EQUAL_UINT32(1U, oldSnapshot.generation);
    TEST_ASSERT_EQUAL_UINT(2U, oldSnapshot.deviceCount);
    TEST_ASSERT_EQUAL_UINT(10U, oldSnapshot.datapointCount);
    TEST_ASSERT_EQUAL_UINT32(49U, owner.snapshot().generation);
}

void test_modbus_owner_quiesce_stops_commands_and_restores_only_prior_state(void) {
    RecordingModbusOwnerBus bus;
    ModbusOwnerCore<ModbusOwnerTestConfiguration, RecordingModbusOwnerBus> owner(bus);
    owner.requestActivation({1, 9600, 1U, 1U});
    TEST_ASSERT_TRUE(owner.processBoundary());
    owner.setActive(true);
    int transactionCalls = 0;
    owner.runPollingPass([&](const ModbusOwnerTestConfiguration &, const size_t) {
        ++transactionCalls;
        TEST_ASSERT_TRUE(owner.requestQuiesceForOta(44U));
    });
    TEST_ASSERT_EQUAL_INT(1, transactionCalls);
    TEST_ASSERT_TRUE(owner.snapshot().quiesced);
    int commandCalls = 0;
    owner.runCommand([&](const ModbusOwnerTestConfiguration &) { ++commandCalls; });
    TEST_ASSERT_EQUAL_INT(0, commandCalls);
    TEST_ASSERT_FALSE(owner.processBoundary());
    TEST_ASSERT_FALSE(owner.resumeFromOta(45U));
    TEST_ASSERT_TRUE(owner.resumeFromOta(44U));
    TEST_ASSERT_TRUE(owner.snapshot().active);

    owner.setActive(false);
    TEST_ASSERT_TRUE(owner.quiesceForOta(46U));
    TEST_ASSERT_TRUE(owner.resumeFromOta(46U));
    TEST_ASSERT_FALSE(owner.snapshot().active);
}

void test_mqtt_owner_snapshots_remain_values_across_replacement(void) {
    constexpr uintptr_t OWNER = 23U;
    RecordingMqttOwnerClient client;
    MqttOwnerCore<RecordingMqttOwnerClient> owner(client, OWNER);
    TEST_ASSERT_TRUE(owner.applyConfiguration(OWNER, "broker-a", true));
    std::vector<MqttOwnerCore<RecordingMqttOwnerClient>::Subscription> initial;
    initial.push_back({"root/old", [](const std::string &) {}});
    TEST_ASSERT_TRUE(owner.replaceSubscriptions(OWNER, std::move(initial), 4U));
    const auto oldSnapshot = owner.snapshot();

    TEST_ASSERT_TRUE(owner.applyConfiguration(OWNER, "broker-b", false));
    std::vector<MqttOwnerCore<RecordingMqttOwnerClient>::Subscription> replacement;
    replacement.push_back({"root/new", [](const std::string &) {}});
    TEST_ASSERT_TRUE(owner.replaceSubscriptions(OWNER, std::move(replacement), 9U));

    TEST_ASSERT_EQUAL_STRING("broker-a", oldSnapshot.server.c_str());
    TEST_ASSERT_TRUE(oldSnapshot.enabled);
    TEST_ASSERT_EQUAL_UINT32(2U, oldSnapshot.generation);
    TEST_ASSERT_EQUAL_UINT32(4U, oldSnapshot.modbusGeneration);
    TEST_ASSERT_EQUAL_UINT(1U, oldSnapshot.topics.size());
    TEST_ASSERT_EQUAL_STRING("root/old", oldSnapshot.topics[0].c_str());
    const auto current = owner.snapshot();
    TEST_ASSERT_EQUAL_STRING("broker-b", current.server.c_str());
    TEST_ASSERT_FALSE(current.enabled);
    TEST_ASSERT_EQUAL_STRING("root/new", current.topics[0].c_str());
}

void test_latest_revision_preserves_exact_candidate_across_overlapping_workers(void) {
    struct Candidate {
        uint32_t revision;
        std::string exactBody;
    };

    LatestRevisionCore revisions;
    const Candidate first{revisions.issue(), R"({"root_topic":"first"})"};
    auto firstAdmission = revisions.beginAdmission(first.revision);
    firstAdmission.commit();
    const Candidate second{revisions.issue(), R"({"root_topic":"second"})"};
    auto secondAdmission = revisions.beginAdmission(second.revision);
    secondAdmission.commit();
    std::string active = "original";

    const auto apply = [&revisions, &active](const Candidate &candidate) {
        if (!revisions.isLatest(candidate.revision)) return false;
        active = candidate.exactBody;
        return true;
    };

    // The later request's worker finishes first. The delayed older worker must
    // report superseded instead of rereading or replacing the newer body.
    TEST_ASSERT_TRUE(apply(second));
    TEST_ASSERT_FALSE(apply(first));
    TEST_ASSERT_EQUAL_STRING(second.exactBody.c_str(), active.c_str());
    TEST_ASSERT_EQUAL_UINT32(2U, revisions.latest());
}

void test_configuration_revision_failure_is_inert_during_concurrent_worker_check(void) {
    LatestRevisionCore revisions;
    const uint32_t acceptedRevision = revisions.issue();
    auto acceptedAdmission = revisions.beginAdmission(acceptedRevision);
    acceptedAdmission.commit();

    // A parsed-file or HTTP-job failure only consumes an inert ticket.
    const uint32_t rejectedBeforeAdmission = revisions.issue();
    TEST_ASSERT_FALSE(revisions.isObsolete(acceptedRevision));
    TEST_ASSERT_EQUAL_UINT32(acceptedRevision, revisions.latest());

    // Reproduce an owner-queue admission attempt while an already admitted
    // worker checks its revision. The reader blocks on the production guard;
    // dropping the admission without commit leaves the older candidate valid.
    std::atomic<bool> observerStarted{false};
    std::atomic<bool> observerFinished{false};
    bool acceptedWorkerMayApply = false;
    std::thread observer;
    bool finishedWhileAdmissionWasPending = false;
    {
        auto rejectedAdmission = revisions.beginAdmission(rejectedBeforeAdmission);
        observer = std::thread([&]() {
            observerStarted.store(true, std::memory_order_release);
            acceptedWorkerMayApply = revisions.isLatest(acceptedRevision);
            observerFinished.store(true, std::memory_order_release);
        });
        while (!observerStarted.load(std::memory_order_acquire)) std::this_thread::yield();
        for (size_t i = 0U; i < 1000U; ++i) std::this_thread::yield();
        finishedWhileAdmissionWasPending = observerFinished.load(std::memory_order_acquire);
        // Simulated queue rejection: no commit.
    }
    observer.join();

    TEST_ASSERT_FALSE(finishedWhileAdmissionWasPending);
    TEST_ASSERT_TRUE(acceptedWorkerMayApply);
    TEST_ASSERT_EQUAL_UINT32(acceptedRevision, revisions.latest());

    const uint32_t replacementRevision = revisions.issue();
    auto replacementAdmission = revisions.beginAdmission(replacementRevision);
    replacementAdmission.commit();
    TEST_ASSERT_TRUE(revisions.isObsolete(acceptedRevision));
    TEST_ASSERT_TRUE(revisions.isLatest(replacementRevision));
}

void test_publication_attempt_tokens_ignore_stale_completions_after_retry(void) {
    PublicationAttemptCore availabilityAttempts;
    bool availabilityPending = true;
    bool availabilityPublished = false;
    const uint32_t abandonedAvailability = availabilityAttempts.begin();

    // Completion admission overflow abandons the first attempt before retry.
    availabilityAttempts.invalidate();
    availabilityPending = false;
    const uint32_t retriedAvailability = availabilityAttempts.begin();
    availabilityPending = true;

    const auto completeAvailability = [&](const uint32_t attempt, const bool success) {
        if (!availabilityPending || !availabilityAttempts.accepts(attempt)) return;
        availabilityPending = false;
        availabilityPublished = success;
    };
    completeAvailability(abandonedAvailability, true);
    TEST_ASSERT_TRUE(availabilityPending);
    TEST_ASSERT_FALSE(availabilityPublished);
    completeAvailability(retriedAvailability, true);
    TEST_ASSERT_FALSE(availabilityPending);
    TEST_ASSERT_TRUE(availabilityPublished);

    PublicationAttemptCore discoveryAttempts;
    uint16_t discoveryPending = 2U;
    bool discoveryFailed = false;
    bool discoveryPublished = false;
    const uint32_t abandonedDiscovery = discoveryAttempts.begin();

    // Reset after one lost completion, then enqueue a complete retry batch.
    discoveryAttempts.invalidate();
    discoveryPending = 0U;
    discoveryFailed = false;
    const uint32_t retriedDiscovery = discoveryAttempts.begin();
    discoveryPending = 2U;

    const auto completeDiscovery = [&](const uint32_t attempt, const bool success) {
        if (discoveryPending == 0U || !discoveryAttempts.accepts(attempt)) return;
        --discoveryPending;
        discoveryFailed = discoveryFailed || !success;
        if (discoveryPending == 0U) discoveryPublished = !discoveryFailed;
    };
    completeDiscovery(abandonedDiscovery, true);
    completeDiscovery(retriedDiscovery, true);
    completeDiscovery(abandonedDiscovery, false);
    TEST_ASSERT_EQUAL_UINT16(1U, discoveryPending);
    TEST_ASSERT_FALSE(discoveryFailed);
    TEST_ASSERT_FALSE(discoveryPublished);
    completeDiscovery(retriedDiscovery, true);
    TEST_ASSERT_EQUAL_UINT16(0U, discoveryPending);
    TEST_ASSERT_TRUE(discoveryPublished);
}

void test_staged_bridge_plan_never_commits_after_running_timeout(void) {
    StagedCommitCore<std::string> bridgePlans;
    OwnerCompletionCore runningRequest;
    TEST_ASSERT_TRUE(runningRequest.markQueued());
    TEST_ASSERT_TRUE(runningRequest.tryStart());

    // Staging may finish after the caller's wait expires, but cancellation can
    // no longer succeed and no approval is published by the Modbus owner.
    bridgePlans.stage("candidate-after-timeout", 8U);
    TEST_ASSERT_FALSE(runningRequest.cancelIfQueued());
    std::string committed;
    uint32_t token = 0U;
    TEST_ASSERT_FALSE(bridgePlans.takeApproved(committed, token));
    TEST_ASSERT_TRUE(committed.empty());

    bridgePlans.stage("coordinated-candidate", 9U);
    bridgePlans.approve(9U);
    TEST_ASSERT_TRUE(bridgePlans.takeApproved(committed, token));
    TEST_ASSERT_EQUAL_UINT32(9U, token);
    TEST_ASSERT_EQUAL_STRING("coordinated-candidate", committed.c_str());
}

void test_async_publications_do_not_wait_for_a_stalled_owner(void) {
    struct Publication {
        std::string payload;
        std::function<void(bool)> completion;
    };

    OwnerMailboxCore<Publication, 4U> publications;
    int callerProgress = 0;
    int completions = 0;
    for (int i = 0; i < 4; ++i) {
        Publication publication;
        publication.payload = "value-" + std::to_string(i);
        publication.completion = [&completions](const bool) { ++completions; };
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(OwnerMailboxCore<Publication, 4U>::Admission::Accepted),
            static_cast<int>(publications.admit(std::move(publication))));
        ++callerProgress;
    }

    // No owner work has run: all polling-side submissions returned and the
    // bounded fifth submission is rejected immediately.
    TEST_ASSERT_EQUAL_INT(4, callerProgress);
    TEST_ASSERT_EQUAL_INT(0, completions);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(OwnerMailboxCore<Publication, 4U>::Admission::Full),
        static_cast<int>(publications.admit(Publication{"overflow", {}})));

    Publication publication;
    while (publications.tryPop(publication)) publication.completion(true);
    TEST_ASSERT_EQUAL_INT(4, completions);
}

void test_production_snapshot_core_never_returns_mixed_runtime_fields(void) {
    struct RuntimeFields {
        uint32_t generation = 0U;
        size_t deviceCount = 0U;
        size_t datapointCount = 0U;
        bool enabled = false;
    };

    CoherentSnapshotCore<RuntimeFields> snapshot;
    std::atomic<bool> finished{false};
    std::atomic<bool> mixed{false};
    std::thread writer([&]() {
        for (uint32_t generation = 1U; generation <= 50000U; ++generation) {
            snapshot.write({generation, generation * 2U, generation * 3U,
                            (generation & 1U) != 0U});
        }
        finished.store(true, std::memory_order_release);
    });

    do {
        const RuntimeFields fields = snapshot.read();
        if (fields.deviceCount != fields.generation * 2U
            || fields.datapointCount != fields.generation * 3U
            || fields.enabled != ((fields.generation & 1U) != 0U)) {
            mixed.store(true, std::memory_order_relaxed);
        }
    } while (!finished.load(std::memory_order_acquire));
    writer.join();
    TEST_ASSERT_FALSE(mixed.load(std::memory_order_relaxed));
}

void test_modbus_mqtt_coil_payload_contract(void) {
    struct CoilCase {
        const char *payload;
        bool accepted;
        uint16_t rawValue;
        ModbusMqttWriteCore::RejectionReason rejection;
    };
    const CoilCase cases[] = {
        {"0", true, 0U, ModbusMqttWriteCore::RejectionReason::None},
        {"1", true, 1U, ModbusMqttWriteCore::RejectionReason::None},
        {" true ", true, 1U, ModbusMqttWriteCore::RejectionReason::None},
        {"\tFaLsE\r\n", true, 0U, ModbusMqttWriteCore::RejectionReason::None},
        {"", false, 0U, ModbusMqttWriteCore::RejectionReason::Empty},
        {" \t", false, 0U, ModbusMqttWriteCore::RejectionReason::Empty},
        {"00", false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {"2", false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {"-1", false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {"1.0", false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {"yes", false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {"true!", false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
    };

    for (const auto &testCase : cases) {
        const auto result = ModbusMqttWriteCore::convertPayload(
            WRITE_COIL, testCase.payload, 1.0);
        TEST_ASSERT_EQUAL(testCase.accepted, result.accepted());
        TEST_ASSERT_EQUAL_UINT16(testCase.rawValue, result.rawValue);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(testCase.rejection),
                              static_cast<int>(result.rejection));
    }
}

void test_modbus_mqtt_holding_payload_grammar_and_conversion(void) {
    struct HoldingCase {
        const char *payload;
        double scale;
        bool accepted;
        uint16_t rawValue;
        ModbusMqttWriteCore::RejectionReason rejection;
    };
    const HoldingCase cases[] = {
        {"0", 1.0, true, 0U, ModbusMqttWriteCore::RejectionReason::None},
        {"+12", 1.0, true, 12U, ModbusMqttWriteCore::RejectionReason::None},
        {"12.5", 1.0, true, 13U, ModbusMqttWriteCore::RejectionReason::None},
        {".5", 1.0, true, 1U, ModbusMqttWriteCore::RejectionReason::None},
        {"1.", 1.0, true, 1U, ModbusMqttWriteCore::RejectionReason::None},
        {"1e3", 1.0, true, 1000U, ModbusMqttWriteCore::RejectionReason::None},
        {" -2.5E-2 ", -0.001, true, 25U, ModbusMqttWriteCore::RejectionReason::None},
        {"12.34", static_cast<double>(0.1F), true, 123U,
         ModbusMqttWriteCore::RejectionReason::None},
        {"-10", static_cast<double>(-0.1F), true, 100U,
         ModbusMqttWriteCore::RejectionReason::None},
        {"65535", 1.0, true, 65535U, ModbusMqttWriteCore::RejectionReason::None},
        {"65535.0000000000000000", 1.0, true, 65535U,
         ModbusMqttWriteCore::RejectionReason::None},
        {"6553.500097654759883880615234375", static_cast<double>(0.1F), true, 65535U,
         ModbusMqttWriteCore::RejectionReason::None},
        {"65534.5", 1.0, true, 65535U, ModbusMqttWriteCore::RejectionReason::None},
        {"1e-9999", 1.0, true, 0U, ModbusMqttWriteCore::RejectionReason::None},
        {"", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::Empty},
        {"+", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {".", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {"1e", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {"12abc", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {"0x10", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {"NaN", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {"Infinity", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidSyntax},
        {"1e9999", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::NonFinite},
        {"1", 0.0, false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidScale},
        {"1", -0.0, false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidScale},
        {"1", INFINITY, false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidScale},
        {"1", NAN, false, 0U, ModbusMqttWriteCore::RejectionReason::InvalidScale},
        {"-0.0001", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::OutOfRange},
        {"-1e-9999", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::OutOfRange},
        {"65535.0001", 1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::OutOfRange},
        {"65535.0000000000000001", 1.0, false, 0U,
         ModbusMqttWriteCore::RejectionReason::OutOfRange},
        {"6553.500097654759883880615234376", static_cast<double>(0.1F), false, 0U,
         ModbusMqttWriteCore::RejectionReason::OutOfRange},
        {"1e-9999", -1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::OutOfRange},
        {"-65535.0000000000000001", -1.0, false, 0U,
         ModbusMqttWriteCore::RejectionReason::OutOfRange},
        {"1", -1.0, false, 0U, ModbusMqttWriteCore::RejectionReason::OutOfRange},
    };

    for (const auto &testCase : cases) {
        const auto result = ModbusMqttWriteCore::convertPayload(
            WRITE_HOLDING, testCase.payload, testCase.scale);
        TEST_ASSERT_EQUAL(testCase.accepted, result.accepted());
        TEST_ASSERT_EQUAL_UINT16(testCase.rawValue, result.rawValue);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(testCase.rejection),
                              static_cast<int>(result.rejection));
    }
}

void test_modbus_mqtt_typed_holding_conversion(void) {
    using ModbusMqttWriteCore::RejectionReason;
    constexpr auto Big = ModbusWordOrder::Big;
    constexpr auto Little = ModbusWordOrder::Little;
    struct TypedCase {
        const char *payload;
        double scale;
        ModbusDataType dataType;
        ModbusWordOrder order;
        ModbusFunctionType function;
        bool accepted;
        uint8_t wordCount;
        uint16_t words[4];
        RejectionReason rejection;
    };
    const double scale001 = static_cast<double>(0.01F);
    const double scale01 = static_cast<double>(0.1F);
    const TypedCase cases[] = {
        // int16: negative values are two's complement; halves round upward.
        {"-2", scale001, INT16, Big, WRITE_HOLDING, true, 1U, {0xFF38U}, RejectionReason::None},
        {"-2", -scale001, INT16, Big, WRITE_HOLDING, true, 1U, {0x00C8U}, RejectionReason::None},
        {"-2.5", 1.0, INT16, Big, WRITE_MULTIPLE_HOLDING, true, 1U, {0xFFFEU}, RejectionReason::None},
        {"-32768", 1.0, INT16, Big, WRITE_HOLDING, true, 1U, {0x8000U}, RejectionReason::None},
        {"32767", 1.0, INT16, Big, WRITE_HOLDING, true, 1U, {0x7FFFU}, RejectionReason::None},
        {"-32769", 1.0, INT16, Big, WRITE_HOLDING, false, 0U, {0U}, RejectionReason::OutOfRange},
        {"32768", 1.0, INT16, Big, WRITE_HOLDING, false, 0U, {0U}, RejectionReason::OutOfRange},
        {"-327.6799", scale001, INT16, Big, WRITE_HOLDING, true, 1U, {0x8000U}, RejectionReason::None},
        {"-327.68", scale001, INT16, Big, WRITE_HOLDING, false, 0U, {0U}, RejectionReason::OutOfRange},
        {"16384", -0.5, INT16, Big, WRITE_HOLDING, true, 1U, {0x8000U}, RejectionReason::None},
        {"-16384", -0.5, INT16, Big, WRITE_HOLDING, false, 0U, {0U}, RejectionReason::OutOfRange},
        // uint16 keeps rejecting negative values.
        {"-1", 1.0, UINT16, Big, WRITE_HOLDING, false, 0U, {0U}, RejectionReason::OutOfRange},
        {"65535", 1.0, UINT16, Little, WRITE_HOLDING, true, 1U, {0xFFFFU}, RejectionReason::None},
        // 32-bit integers occupy two registers in the configured word order.
        {"4294967295", 1.0, UINT32, Big, WRITE_MULTIPLE_HOLDING, true, 2U, {0xFFFFU, 0xFFFFU},
         RejectionReason::None},
        {"4294967296", 1.0, UINT32, Big, WRITE_MULTIPLE_HOLDING, false, 0U, {0U},
         RejectionReason::OutOfRange},
        {"-1", 1.0, UINT32, Big, WRITE_MULTIPLE_HOLDING, false, 0U, {0U}, RejectionReason::OutOfRange},
        {"65536", 1.0, UINT32, Little, WRITE_MULTIPLE_HOLDING, true, 2U, {0x0000U, 0x0001U},
         RejectionReason::None},
        {"-2147483648", 1.0, INT32, Big, WRITE_MULTIPLE_HOLDING, true, 2U, {0x8000U, 0x0000U},
         RejectionReason::None},
        {"-2147483648", 1.0, INT32, Little, WRITE_MULTIPLE_HOLDING, true, 2U, {0x0000U, 0x8000U},
         RejectionReason::None},
        {"2147483647", 1.0, INT32, Big, WRITE_MULTIPLE_HOLDING, true, 2U, {0x7FFFU, 0xFFFFU},
         RejectionReason::None},
        {"2147483648", 1.0, INT32, Big, WRITE_MULTIPLE_HOLDING, false, 0U, {0U},
         RejectionReason::OutOfRange},
        {"-2147483649", 1.0, INT32, Big, WRITE_MULTIPLE_HOLDING, false, 0U, {0U},
         RejectionReason::OutOfRange},
        {"-100000", 1.0, INT32, Big, WRITE_MULTIPLE_HOLDING, true, 2U, {0xFFFEU, 0x7960U},
         RejectionReason::None},
        {"-1000", scale01, INT32, Big, WRITE_MULTIPLE_HOLDING, true, 2U, {0xFFFFU, 0xD8F0U},
         RejectionReason::None},
        // 64-bit integers occupy four registers; the exact decimal check still
        // applies beyond double precision.
        {"9223372036854775807", 1.0, INT64, Big, WRITE_MULTIPLE_HOLDING, true, 4U,
         {0x7FFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU}, RejectionReason::None},
        {"9223372036854775808", 1.0, INT64, Big, WRITE_MULTIPLE_HOLDING, false, 0U, {0U},
         RejectionReason::OutOfRange},
        {"-9223372036854775808", 1.0, INT64, Big, WRITE_MULTIPLE_HOLDING, true, 4U,
         {0x8000U, 0x0000U, 0x0000U, 0x0000U}, RejectionReason::None},
        {"-9223372036854775809", 1.0, INT64, Big, WRITE_MULTIPLE_HOLDING, false, 0U, {0U},
         RejectionReason::OutOfRange},
        {"18446744073709551615", 1.0, UINT64, Big, WRITE_MULTIPLE_HOLDING, true, 4U,
         {0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU}, RejectionReason::None},
        {"18446744073709551616", 1.0, UINT64, Big, WRITE_MULTIPLE_HOLDING, false, 0U, {0U},
         RejectionReason::OutOfRange},
        {"1", 1.0, UINT64, Little, WRITE_MULTIPLE_HOLDING, true, 4U, {0x0001U, 0U, 0U, 0U},
         RejectionReason::None},
        // float32: raw must fit a float; no rounding to integers.
        {"3.14", 1.0, FLOAT32, Big, WRITE_MULTIPLE_HOLDING, true, 2U, {0x4048U, 0xF5C3U},
         RejectionReason::None},
        {"3.14", 1.0, FLOAT32, Little, WRITE_MULTIPLE_HOLDING, true, 2U, {0xF5C3U, 0x4048U},
         RejectionReason::None},
        {"-100", scale01, FLOAT32, Big, WRITE_MULTIPLE_HOLDING, true, 2U, {0xC47AU, 0x0000U},
         RejectionReason::None},
        {"1e39", 1.0, FLOAT32, Big, WRITE_MULTIPLE_HOLDING, false, 0U, {0U},
         RejectionReason::OutOfRange},
        {"1e-50", 1.0, FLOAT32, Big, WRITE_MULTIPLE_HOLDING, true, 2U, {0x0000U, 0x0000U},
         RejectionReason::None},
        {"nan", 1.0, FLOAT32, Big, WRITE_MULTIPLE_HOLDING, false, 0U, {0U},
         RejectionReason::InvalidSyntax},
        // FC6 writes one register: 16-bit types only. TEXT is never a number.
        {"1", 1.0, UINT16, Big, WRITE_HOLDING, true, 1U, {0x0001U}, RejectionReason::None},
        {"1", 1.0, INT32, Big, WRITE_HOLDING, false, 0U, {0U}, RejectionReason::UnsupportedDataType},
        {"1", 1.0, FLOAT32, Big, WRITE_HOLDING, false, 0U, {0U}, RejectionReason::UnsupportedDataType},
        {"1", 1.0, TEXT, Big, WRITE_MULTIPLE_HOLDING, false, 0U, {0U},
         RejectionReason::UnsupportedDataType},
    };

    for (const auto &testCase : cases) {
        const auto result = ModbusMqttWriteCore::convertPayload(
            testCase.function, testCase.payload, testCase.scale, testCase.dataType, testCase.order);
        TEST_ASSERT_EQUAL_MESSAGE(testCase.accepted, result.accepted(), testCase.payload);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(testCase.rejection),
                                      static_cast<int>(result.rejection), testCase.payload);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(testCase.wordCount, result.wordCount, testCase.payload);
        for (uint8_t index = 0U; index < testCase.wordCount; ++index) {
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(testCase.words[index], result.words[index],
                                             testCase.payload);
        }
        if (testCase.accepted) {
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(testCase.words[0], result.rawValue, testCase.payload);
        }
    }
    TEST_ASSERT_EQUAL_STRING("unsupported_data_type",
                             ModbusMqttWriteCore::rejectionReasonToString(
                                 RejectionReason::UnsupportedDataType));
}

void test_modbus_mqtt_production_handler_submits_only_accepted_commands(void) {
    ModbusMqttWriteCore::CommandContext context;
    context.slaveId = 7U;
    context.function = WRITE_MULTIPLE_HOLDING;
    context.address = 321U;
    context.registerCount = 8U;
    context.scale = 1.0;
    context.dataType = INT32;
    context.wordOrder = ModbusWordOrder::Big;
    context.modbusGeneration = 42U;

    std::vector<ModbusMqttWriteCore::WriteCommand> submissions;
    std::vector<std::string> warnings;
    const auto handler = ModbusMqttWriteCore::buildMessageHandler(
        context,
        [&](const ModbusMqttWriteCore::WriteCommand &command) { submissions.push_back(command); },
        [&](const std::string &warning) { warnings.push_back(warning); });

    handler("root/device/write/set", "secret-invalid-payload");
    TEST_ASSERT_TRUE(submissions.empty());
    TEST_ASSERT_EQUAL_UINT(1U, warnings.size());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[0].find("topic=root/device/write/set"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[0].find("function=16"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[0].find("address=321"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[0].find("reason=invalid_syntax"));
    TEST_ASSERT_EQUAL(std::string::npos, warnings[0].find("secret-invalid-payload"));

    handler("root/device/write/set", "-2147483648.5");
    handler("root/device/write/set", "2147483647.0000000000000001");
    TEST_ASSERT_TRUE(submissions.empty());
    TEST_ASSERT_EQUAL_UINT(3U, warnings.size());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[1].find("reason=out_of_range"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[2].find("reason=out_of_range"));
    TEST_ASSERT_EQUAL(std::string::npos, warnings[1].find("-2147483648.5"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      warnings[2].find("2147483647.0000000000000001"));

    handler("root/device/write/set", "10");
    TEST_ASSERT_EQUAL_UINT(1U, submissions.size());
    TEST_ASSERT_EQUAL_STRING("root/device/write/set", submissions[0].sourceTopic.c_str());
    TEST_ASSERT_EQUAL_UINT8(7U, submissions[0].slaveId);
    TEST_ASSERT_EQUAL_INT(16, static_cast<int>(submissions[0].function));
    TEST_ASSERT_EQUAL_UINT16(321U, submissions[0].address);
    TEST_ASSERT_EQUAL_UINT8(2U, submissions[0].registerCount);
    TEST_ASSERT_EQUAL_UINT8(2U, submissions[0].wordCount);
    TEST_ASSERT_EQUAL_UINT16(0U, submissions[0].words[0]);
    TEST_ASSERT_EQUAL_UINT16(10U, submissions[0].words[1]);
    TEST_ASSERT_FALSE(submissions[0].coilValue);
    TEST_ASSERT_EQUAL_UINT32(42U, submissions[0].modbusGeneration);

    handler("root/device/write/set", "4294967296");
    handler("root/device/write/set", "-1");
    TEST_ASSERT_EQUAL_UINT(2U, submissions.size());
    TEST_ASSERT_EQUAL_UINT16(0xFFFFU, submissions[1].words[0]);
    TEST_ASSERT_EQUAL_UINT16(0xFFFFU, submissions[1].words[1]);
    TEST_ASSERT_EQUAL_UINT(4U, warnings.size());

    // Coils keep the configured register count and carry the state as a flag.
    ModbusMqttWriteCore::CommandContext coilContext;
    coilContext.function = WRITE_COIL;
    coilContext.address = 5U;
    coilContext.registerCount = 1U;
    const auto coilHandler = ModbusMqttWriteCore::buildMessageHandler(
        coilContext,
        [&](const ModbusMqttWriteCore::WriteCommand &command) { submissions.push_back(command); },
        [&](const std::string &warning) { warnings.push_back(warning); });
    coilHandler("root/device/coil/set", "true");
    coilHandler("root/device/coil/set", "0");
    TEST_ASSERT_EQUAL_UINT(4U, submissions.size());
    TEST_ASSERT_TRUE(submissions[2].coilValue);
    TEST_ASSERT_FALSE(submissions[3].coilValue);
    TEST_ASSERT_EQUAL_UINT8(1U, submissions[2].registerCount);
    TEST_ASSERT_EQUAL_UINT8(0U, submissions[2].wordCount);
}

std::string modbusConfigurationWithScale(const int function, const char *scale,
                                         const char *dataType = "uint16",
                                         const int numOfRegisters = 1,
                                         const char *extraFields = "") {
    return std::string(R"({"version":1,"bus":{"enabled":true,"baud":9600,"serialFormat":"8N1"},"devices":[{"id":"device","name":"Device","slaveId":1,"dataPoints":[{"id":"point","name":"Point","function":)")
           + std::to_string(function)
           + R"(,"address":10,"numOfRegisters":)" + std::to_string(numOfRegisters)
           + R"(,"scale":)" + scale
           + R"(,"dataType":")" + dataType + R"(","unit":"","topic":"write")"
           + extraFields + "}]}]}";
}

void test_modbus_configuration_coerces_register_shape_per_data_type(void) {
    using ModbusConfigDocument::ParseWarning;
    ConfigurationRoot parsed;
    std::vector<ModbusConfigDocument::ParseNotice> notices;
    const auto parseOk = [&](const std::string &json) {
        return ModbusConfigDocument::parse(json.c_str(), json.size(), parsed, nullptr, &notices);
    };
    const auto point = [&]() -> const ModbusDatapoint & { return parsed.devices[0].datapoints[0]; };

    // FC3 int32 with one register is coerced up to the type's width (boot-safe).
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(3, "1", "int32", 1)));
    TEST_ASSERT_EQUAL_UINT8(2U, point().numOfRegisters);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusWordOrder::Big), static_cast<int>(point().wordOrder));
    TEST_ASSERT_EQUAL_UINT(1U, notices.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseWarning::RegisterCountCoerced),
                          static_cast<int>(notices[0].warning));
    TEST_ASSERT_EQUAL_UINT(0U, notices[0].deviceIndex);
    TEST_ASSERT_EQUAL_UINT(0U, notices[0].datapointIndex);
    TEST_ASSERT_EQUAL_UINT16(1U, notices[0].previous);
    TEST_ASSERT_EQUAL_UINT16(2U, notices[0].coerced);

    // A longer read than the width is kept; the decoder uses the first words.
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(4, "1", "uint16", 3)));
    TEST_ASSERT_EQUAL_UINT8(3U, point().numOfRegisters);
    TEST_ASSERT_TRUE(notices.empty());

    // FC16 always writes exactly the type's width.
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(16, "1", "int64", 1)));
    TEST_ASSERT_EQUAL_UINT8(4U, point().numOfRegisters);
    TEST_ASSERT_EQUAL_UINT(1U, notices.size());
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(16, "0.5", "uint16", 3)));
    TEST_ASSERT_EQUAL_UINT8(1U, point().numOfRegisters);
    TEST_ASSERT_EQUAL_UINT(1U, notices.size());
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(16, "0.5", "float32", 2)));
    TEST_ASSERT_EQUAL_UINT8(2U, point().numOfRegisters);
    TEST_ASSERT_TRUE(notices.empty());

    // FC6 with a 16-bit type is coerced to one register.
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(6, "1", "int16", 2)));
    TEST_ASSERT_EQUAL_UINT8(1U, point().numOfRegisters);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(INT16), static_cast<int>(point().dataType));
    TEST_ASSERT_EQUAL_UINT(1U, notices.size());

    // FC6 with a multi-register type is rejected atomically, like an invalid scale.
    const char *wideTypes[] = {"int32", "uint32", "float32", "int64", "uint64"};
    for (const char *wideType : wideTypes) {
        ConfigurationRoot active;
        active.bus.baud = 19200;
        active.bus.enabled = true;
        ModbusDevice existing{};
        existing.id = "existing";
        active.devices.push_back(existing);
        ModbusConfigDocument::ValidationError error;
        const std::string json = modbusConfigurationWithScale(6, "1", wideType, 2);
        TEST_ASSERT_FALSE(ModbusConfigDocument::parse(json.c_str(), json.size(), active, &error, &notices));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusConfigDocument::ValidationReason::InvalidRegisterCount),
                              static_cast<int>(error.reason));
        TEST_ASSERT_EQUAL_UINT(0U, error.deviceIndex);
        TEST_ASSERT_EQUAL_UINT(0U, error.datapointIndex);
        TEST_ASSERT_EQUAL_INT(19200, active.bus.baud);
        TEST_ASSERT_EQUAL_UINT(1U, active.devices.size());
        TEST_ASSERT_EQUAL_STRING("existing", active.devices[0].id.c_str());
    }
    TEST_ASSERT_EQUAL_STRING("invalid_register_count",
                             ModbusConfigDocument::reasonToString(
                                 ModbusConfigDocument::ValidationReason::InvalidRegisterCount));

    // At boot (lenient mode) the same combination keeps loading as uint16,
    // the encoding every FC6 value had before data types were honoured, so a
    // datapoint saved by an earlier firmware neither stops Modbus on every
    // device nor changes its accepted range.
    struct Narrowing {
        const char *name;
        ModbusDataType wide;
        ModbusDataType narrowed;
    };
    const Narrowing narrowings[] = {
        {"int32", INT32, UINT16},   {"int64", INT64, UINT16},     {"uint32", UINT32, UINT16},
        {"uint64", UINT64, UINT16}, {"float32", FLOAT32, UINT16},
    };
    for (const auto &narrowing : narrowings) {
        ModbusConfigDocument::ValidationError error;
        const std::string json = modbusConfigurationWithScale(6, "0.1", narrowing.name, 1);
        TEST_ASSERT_TRUE(ModbusConfigDocument::parse(json.c_str(), json.size(), parsed, &error, &notices,
                                                     ModbusConfigDocument::ParseMode::Lenient));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusConfigDocument::ValidationReason::None),
                              static_cast<int>(error.reason));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(narrowing.narrowed), static_cast<int>(point().dataType));
        TEST_ASSERT_EQUAL_UINT8(1U, point().numOfRegisters);
        TEST_ASSERT_EQUAL_UINT(1U, notices.size());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseWarning::DataTypeCoercedToUint16),
                              static_cast<int>(notices[0].warning));
        TEST_ASSERT_EQUAL_UINT(0U, notices[0].deviceIndex);
        TEST_ASSERT_EQUAL_UINT(0U, notices[0].datapointIndex);
        TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(narrowing.wide), notices[0].previous);
        TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(narrowing.narrowed), notices[0].coerced);
        // The upgrade path keeps the v8.0.4 write behaviour: 0..65535 x scale.
        const ModbusMqttWriteCore::NumberDiscoveryBounds bounds =
            ModbusMqttWriteCore::numberDiscoveryBounds(static_cast<double>(point().scale), point().dataType);
        TEST_ASSERT_TRUE(bounds.valid());
        TEST_ASSERT_TRUE(std::fabs(bounds.minimum) < 0.000001);
        TEST_ASSERT_TRUE(std::fabs(bounds.maximum - 6553.5) < 0.001);
        TEST_ASSERT_TRUE(ModbusMqttWriteCore::convertPayload(WRITE_HOLDING, "4000", point().scale,
                                                             point().dataType, point().wordOrder).accepted());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusMqttWriteCore::RejectionReason::OutOfRange),
                              static_cast<int>(ModbusMqttWriteCore::convertPayload(
                                                   WRITE_HOLDING, "-0.5", point().scale, point().dataType,
                                                   point().wordOrder).rejection));
    }
    TEST_ASSERT_EQUAL_STRING("data_type_coerced_to_uint16",
                             ModbusConfigDocument::warningToString(ParseWarning::DataTypeCoercedToUint16));
    // Lenient mode changes nothing else: FC16 keeps its wide type and width.
    {
        const std::string json = modbusConfigurationWithScale(16, "1", "int32", 2);
        TEST_ASSERT_TRUE(ModbusConfigDocument::parse(json.c_str(), json.size(), parsed, nullptr, &notices,
                                                     ModbusConfigDocument::ParseMode::Lenient));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(INT32), static_cast<int>(point().dataType));
        TEST_ASSERT_EQUAL_UINT8(2U, point().numOfRegisters);
        TEST_ASSERT_TRUE(notices.empty());
    }

    // wordOrder: "big" by default, "little"/1 selects little word order.
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(3, "1", "uint32", 2, R"(,"wordOrder":"little")")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusWordOrder::Little), static_cast<int>(point().wordOrder));
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(3, "1", "uint32", 2, R"(,"wordOrder":"LITTLE")")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusWordOrder::Little), static_cast<int>(point().wordOrder));
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(3, "1", "uint32", 2, R"(,"wordOrder":1)")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusWordOrder::Little), static_cast<int>(point().wordOrder));
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(3, "1", "uint32", 2, R"(,"wordOrder":"big")")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusWordOrder::Big), static_cast<int>(point().wordOrder));
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(3, "1", "uint32", 2, R"(,"wordOrder":"sideways")")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusWordOrder::Big), static_cast<int>(point().wordOrder));
    TEST_ASSERT_TRUE(notices.empty());

    // Byte slices only apply to 16-bit types.
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(3, "1", "int32", 2, R"(,"registerSlice":"low_byte")")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(RegisterSlice::Full), static_cast<int>(point().registerSlice));
    TEST_ASSERT_EQUAL_UINT(1U, notices.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseWarning::RegisterSliceIgnored),
                          static_cast<int>(notices[0].warning));
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(3, "1", "int16", 1, R"(,"registerSlice":"low_byte")")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(RegisterSlice::LowByte), static_cast<int>(point().registerSlice));
    TEST_ASSERT_TRUE(notices.empty());

    // TEXT on a register write becomes uint16; TEXT reads keep their length.
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(6, "1", "TEXT", 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(UINT16), static_cast<int>(point().dataType));
    TEST_ASSERT_EQUAL_UINT(1U, notices.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseWarning::TextWriteCoercedToUint16),
                          static_cast<int>(notices[0].warning));
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(3, "1", "TEXT", 10)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TEXT), static_cast<int>(point().dataType));
    TEST_ASSERT_EQUAL_UINT8(10U, point().numOfRegisters);
    TEST_ASSERT_TRUE(notices.empty());

    // Bit functions are not register functions: nothing is coerced.
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(1, "1", "int32", 1)));
    TEST_ASSERT_EQUAL_UINT8(1U, point().numOfRegisters);
    TEST_ASSERT_TRUE(notices.empty());
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(2, "1", "uint16", 16)));
    TEST_ASSERT_EQUAL_UINT8(16U, point().numOfRegisters);
    TEST_ASSERT_TRUE(notices.empty());

    // More than 125 registers is clamped instead of wrapping through uint8_t.
    TEST_ASSERT_TRUE(parseOk(modbusConfigurationWithScale(3, "1", "TEXT", 300)));
    TEST_ASSERT_EQUAL_UINT8(125U, point().numOfRegisters);
    TEST_ASSERT_EQUAL_UINT(1U, notices.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseWarning::RegisterCountClamped),
                          static_cast<int>(notices[0].warning));
    TEST_ASSERT_EQUAL_UINT16(300U, notices[0].previous);
    TEST_ASSERT_EQUAL_UINT16(125U, notices[0].coerced);
}

void test_modbus_configuration_rejects_noninvertible_writable_scales_atomically(void) {
    const char *invalidScales[] = {"0", "-0.0", "1e100", "\"not-a-number\""};
    for (const char *scale : invalidScales) {
        ConfigurationRoot active;
        active.bus.baud = 19200;
        active.bus.serialFormat = "7E1";
        active.bus.enabled = true;
        ModbusDevice existing{};
        existing.id = "existing";
        active.devices.push_back(existing);

        ModbusConfigDocument::ValidationError error;
        const std::string json = modbusConfigurationWithScale(6, scale);
        TEST_ASSERT_FALSE(ModbusConfigDocument::parse(json.c_str(), json.size(), active, &error));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusConfigDocument::ValidationReason::InvalidScale),
                              static_cast<int>(error.reason));
        TEST_ASSERT_EQUAL_UINT(0U, error.deviceIndex);
        TEST_ASSERT_EQUAL_UINT(0U, error.datapointIndex);
        TEST_ASSERT_EQUAL_INT(19200, active.bus.baud);
        TEST_ASSERT_EQUAL_STRING("7E1", active.bus.serialFormat.c_str());
        TEST_ASSERT_TRUE(active.bus.enabled);
        TEST_ASSERT_EQUAL_UINT(1U, active.devices.size());
        TEST_ASSERT_EQUAL_STRING("existing", active.devices[0].id.c_str());
    }

    const std::string negative = modbusConfigurationWithScale(16, "-0.25");
    ConfigurationRoot accepted;
    TEST_ASSERT_TRUE(ModbusConfigDocument::parse(negative.c_str(), negative.size(), accepted));
    TEST_ASSERT_EQUAL_UINT(1U, accepted.devices.size());
    TEST_ASSERT_EQUAL_UINT(1U, accepted.devices[0].datapoints.size());
    TEST_ASSERT_TRUE(std::fabs(accepted.devices[0].datapoints[0].scale + 0.25F) < 0.000001F);

    const std::string positive = modbusConfigurationWithScale(6, "0.5");
    TEST_ASSERT_TRUE(ModbusConfigDocument::parse(positive.c_str(), positive.size(), accepted));
    TEST_ASSERT_TRUE(std::fabs(accepted.devices[0].datapoints[0].scale - 0.5F) < 0.000001F);

    const std::string coilZero = modbusConfigurationWithScale(5, "0");
    TEST_ASSERT_TRUE(ModbusConfigDocument::parse(coilZero.c_str(), coilZero.size(), accepted));
    const std::string readZero = modbusConfigurationWithScale(3, "0");
    TEST_ASSERT_TRUE(ModbusConfigDocument::parse(readZero.c_str(), readZero.size(), accepted));
}

void test_modbus_home_assistant_number_bounds_follow_scale_direction(void) {
    const double positiveScale = static_cast<double>(0.1F);
    const auto positive = ModbusMqttWriteCore::numberDiscoveryBounds(positiveScale, UINT16);
    TEST_ASSERT_TRUE(positive.valid());
    TEST_ASSERT_TRUE(std::fabs(positive.minimum) < 0.0000001);
    TEST_ASSERT_TRUE(std::fabs(positive.maximum - (65535.0 * positiveScale)) < 0.000001);
    TEST_ASSERT_TRUE(std::fabs(positive.step - positiveScale) < 0.0000001);

    const double negativeScale = static_cast<double>(-0.25F);
    const auto negative = ModbusMqttWriteCore::numberDiscoveryBounds(negativeScale, UINT16);
    TEST_ASSERT_TRUE(negative.valid());
    TEST_ASSERT_TRUE(std::fabs(negative.minimum - (65535.0 * negativeScale)) < 0.000001);
    TEST_ASSERT_TRUE(std::fabs(negative.maximum) < 0.0000001);
    TEST_ASSERT_TRUE(std::fabs(negative.step - 0.25) < 0.0000001);

    TEST_ASSERT_FALSE(ModbusMqttWriteCore::numberDiscoveryBounds(0.0, UINT16).valid());
    TEST_ASSERT_FALSE(ModbusMqttWriteCore::numberDiscoveryBounds(INFINITY, UINT16).valid());
    TEST_ASSERT_FALSE(ModbusMqttWriteCore::numberDiscoveryBounds(NAN, UINT16).valid());

    // Signed types span both signs; a negative scale mirrors the range.
    const double centi = static_cast<double>(0.01F);
    const auto int16Bounds = ModbusMqttWriteCore::numberDiscoveryBounds(centi, INT16);
    TEST_ASSERT_TRUE(int16Bounds.valid());
    TEST_ASSERT_TRUE(std::fabs(int16Bounds.minimum - (-32768.0 * centi)) < 0.000001);
    TEST_ASSERT_TRUE(std::fabs(int16Bounds.maximum - (32767.0 * centi)) < 0.000001);
    TEST_ASSERT_TRUE(std::fabs(int16Bounds.step - centi) < 0.0000001);
    const auto mirrored = ModbusMqttWriteCore::numberDiscoveryBounds(-centi, INT16);
    TEST_ASSERT_TRUE(mirrored.valid());
    TEST_ASSERT_TRUE(std::fabs(mirrored.minimum - (-32767.0 * centi)) < 0.000001);
    TEST_ASSERT_TRUE(std::fabs(mirrored.maximum - (32768.0 * centi)) < 0.000001);
    TEST_ASSERT_TRUE(std::fabs(mirrored.step - centi) < 0.0000001);

    const auto uint32Bounds = ModbusMqttWriteCore::numberDiscoveryBounds(1.0, UINT32);
    TEST_ASSERT_TRUE(uint32Bounds.valid());
    TEST_ASSERT_TRUE(std::fabs(uint32Bounds.minimum) < 0.0000001);
    TEST_ASSERT_TRUE(std::fabs(uint32Bounds.maximum - 4294967295.0) < 0.000001);
    TEST_ASSERT_TRUE(std::fabs(uint32Bounds.step - 1.0) < 0.0000001);
    const auto int64Bounds = ModbusMqttWriteCore::numberDiscoveryBounds(1.0, INT64);
    TEST_ASSERT_TRUE(int64Bounds.valid());
    TEST_ASSERT_TRUE(int64Bounds.minimum < -9.2e18 && int64Bounds.maximum > 9.2e18);

    const auto floatBounds = ModbusMqttWriteCore::numberDiscoveryBounds(1.0, FLOAT32);
    TEST_ASSERT_TRUE(floatBounds.valid());
    TEST_ASSERT_TRUE(floatBounds.minimum == -static_cast<double>(std::numeric_limits<float>::max()));
    TEST_ASSERT_TRUE(floatBounds.maximum == static_cast<double>(std::numeric_limits<float>::max()));
    TEST_ASSERT_TRUE(std::fabs(floatBounds.step - 0.001) < 0.0000001);

    // Home Assistant accepts no step below 0.001.
    const auto fineBounds = ModbusMqttWriteCore::numberDiscoveryBounds(0.0001, UINT16);
    TEST_ASSERT_TRUE(fineBounds.valid());
    TEST_ASSERT_TRUE(std::fabs(fineBounds.step - 0.001) < 0.0000001);
    TEST_ASSERT_TRUE(std::fabs(fineBounds.maximum - 6.5535) < 0.000001);

    const auto textBounds = ModbusMqttWriteCore::numberDiscoveryBounds(1.0, TEXT);
    TEST_ASSERT_FALSE(textBounds.valid());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusMqttWriteCore::RejectionReason::UnsupportedDataType),
                          static_cast<int>(textBounds.rejection));
}

void test_modbus_command_topics_derive_from_the_state_base(void) {
    ModbusDevice device{};
    device.id = "boiler";
    device.name = "Boiler";
    device.slaveId = 10U;
    ModbusDatapoint datapoint{};
    datapoint.id = "boiler.setpoint";
    datapoint.name = "Setpoint";
    datapoint.function = WRITE_HOLDING;

    const ModbusTopicBuilder relative("");
    TEST_ASSERT_EQUAL_STRING("boiler/setpoint", relative.datapointTopic(device, datapoint).c_str());
    TEST_ASSERT_EQUAL_STRING("boiler/setpoint/set", relative.commandTopic(device, datapoint).c_str());

    const ModbusTopicBuilder rooted("mbx/");
    TEST_ASSERT_EQUAL_STRING("mbx/boiler/setpoint", rooted.datapointTopic(device, datapoint).c_str());
    TEST_ASSERT_EQUAL_STRING("mbx/boiler/setpoint/set", rooted.commandTopic(device, datapoint).c_str());

    // An explicit topic is a base too: the root is not prepended and "/set" is
    // appended once, regardless of surrounding whitespace or trailing slashes.
    datapoint.topic = " plant/boiler/sp/ ";
    TEST_ASSERT_EQUAL_STRING("plant/boiler/sp/set", rooted.commandTopic(device, datapoint).c_str());
    TEST_ASSERT_EQUAL_STRING("a/b/set", ModbusTopicBuilder::commandTopicForBase("a/b//").c_str());
    TEST_ASSERT_EQUAL_STRING("", ModbusTopicBuilder::commandTopicForBase("  ").c_str());
}

void test_modbus_codec_decodes_each_data_type(void) {
    using namespace ModbusValueCodec;
    constexpr auto Big = ModbusWordOrder::Big;
    constexpr auto Little = ModbusWordOrder::Little;
    constexpr auto Full = RegisterSlice::Full;
    char text[64];

    TEST_ASSERT_EQUAL_UINT8(0U, wordWidth(TEXT));
    TEST_ASSERT_EQUAL_UINT8(1U, wordWidth(INT16));
    TEST_ASSERT_EQUAL_UINT8(1U, wordWidth(UINT16));
    TEST_ASSERT_EQUAL_UINT8(2U, wordWidth(INT32));
    TEST_ASSERT_EQUAL_UINT8(2U, wordWidth(UINT32));
    TEST_ASSERT_EQUAL_UINT8(2U, wordWidth(FLOAT32));
    TEST_ASSERT_EQUAL_UINT8(4U, wordWidth(INT64));
    TEST_ASSERT_EQUAL_UINT8(4U, wordWidth(UINT64));

    // Nilan CTS602 outdoor temperature (issue #25): input register 208 holds
    // int16 0xFF38 = -200 raw at scale 0.01 -> -2.00 degrees, not 653.36.
    const uint16_t negative[] = {0xFF38U};
    DecodedValue decoded = decode(negative, 1U, INT16, Full, Big);
    TEST_ASSERT_TRUE(decoded.ok);
    TEST_ASSERT_TRUE(decoded.rawSigned == -200);
    TEST_ASSERT_TRUE(std::fabs(decoded.raw + 200.0) < 0.000001);
    formatEngineering(engineeringValue(decoded, 0.01F), INT16, 0.01F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("-2.00", text);

    decoded = decode(negative, 1U, UINT16, Full, Big);
    TEST_ASSERT_TRUE(decoded.ok);
    TEST_ASSERT_TRUE(decoded.rawUnsigned == 65336U);
    formatEngineering(engineeringValue(decoded, 0.01F), UINT16, 0.01F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("653.36", text);

    const uint16_t extreme[] = {0x8000U};
    TEST_ASSERT_TRUE(decode(extreme, 1U, UINT16, Full, Big).rawUnsigned == 32768U);
    TEST_ASSERT_TRUE(decode(extreme, 1U, INT16, Full, Big).rawSigned == -32768);

    const uint16_t int32Big[] = {0xFFFFU, 0xFF38U};
    TEST_ASSERT_TRUE(decode(int32Big, 2U, INT32, Full, Big).rawSigned == -200);
    const uint16_t int32Little[] = {0xFF38U, 0xFFFFU};
    TEST_ASSERT_TRUE(decode(int32Little, 2U, INT32, Full, Little).rawSigned == -200);
    TEST_ASSERT_TRUE(decode(int32Big, 2U, UINT32, Full, Big).rawUnsigned == 4294967096U);

    const uint16_t uint32Words[] = {0x0001U, 0x0000U};
    TEST_ASSERT_TRUE(decode(uint32Words, 2U, UINT32, Full, Big).rawUnsigned == 65536U);
    TEST_ASSERT_TRUE(decode(uint32Words, 2U, UINT32, Full, Little).rawUnsigned == 1U);
    TEST_ASSERT_TRUE(decode(uint32Words, 2U, INT32, Full, Big).rawSigned == 65536);

    const uint16_t pi[] = {0x4048U, 0xF5C3U};  // 3.14f
    decoded = decode(pi, 2U, FLOAT32, Full, Big);
    TEST_ASSERT_TRUE(decoded.ok);
    TEST_ASSERT_TRUE(std::fabs(decoded.rawFloat - 3.14F) < 0.00001F);
    const uint16_t piLittle[] = {0xF5C3U, 0x4048U};
    TEST_ASSERT_TRUE(std::fabs(decode(piLittle, 2U, FLOAT32, Full, Little).rawFloat - 3.14F) < 0.00001F);
    const uint16_t minusHundred[] = {0xC2C8U, 0x0000U};  // -100.0f
    TEST_ASSERT_TRUE(std::fabs(decode(minusHundred, 2U, FLOAT32, Full, Big).raw + 100.0) < 0.000001);

    const uint16_t int64Max[] = {0x7FFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU};
    TEST_ASSERT_TRUE(decode(int64Max, 4U, INT64, Full, Big).rawSigned
                     == std::numeric_limits<int64_t>::max());
    const uint16_t int64Min[] = {0x8000U, 0U, 0U, 0U};
    TEST_ASSERT_TRUE(decode(int64Min, 4U, INT64, Full, Big).rawSigned
                     == std::numeric_limits<int64_t>::min());
    const uint16_t int64MinLittle[] = {0U, 0U, 0U, 0x8000U};
    TEST_ASSERT_TRUE(decode(int64MinLittle, 4U, INT64, Full, Little).rawSigned
                     == std::numeric_limits<int64_t>::min());
    const uint16_t uint64Max[] = {0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU};
    TEST_ASSERT_TRUE(decode(uint64Max, 4U, UINT64, Full, Big).rawUnsigned
                     == std::numeric_limits<uint64_t>::max());
    const uint16_t counted[] = {0x0001U, 0x0002U, 0x0003U, 0x0004U};
    TEST_ASSERT_TRUE(decode(counted, 4U, UINT64, Full, Big).rawUnsigned == 0x0001000200030004ULL);
    TEST_ASSERT_TRUE(decode(counted, 4U, UINT64, Full, Little).rawUnsigned == 0x0004000300020001ULL);

    // Byte slices apply to the 16-bit types only; int16 reads the byte as int8.
    const uint16_t sliced[] = {0x1280U};
    TEST_ASSERT_TRUE(decode(sliced, 1U, INT16, RegisterSlice::LowByte, Big).rawSigned == -128);
    TEST_ASSERT_TRUE(decode(sliced, 1U, INT16, RegisterSlice::HighByte, Big).rawSigned == 18);
    TEST_ASSERT_TRUE(decode(sliced, 1U, UINT16, RegisterSlice::LowByte, Big).rawUnsigned == 128U);
    TEST_ASSERT_TRUE(decode(sliced, 1U, UINT16, RegisterSlice::HighByte, Big).rawUnsigned == 18U);
    TEST_ASSERT_TRUE(decode(uint32Words, 2U, UINT32, RegisterSlice::LowByte, Big).rawUnsigned == 65536U);

    // Too few words or TEXT never decode; extra words are ignored.
    TEST_ASSERT_FALSE(decode(uint32Words, 1U, INT32, Full, Big).ok);
    TEST_ASSERT_FALSE(decode(counted, 3U, INT64, Full, Big).ok);
    TEST_ASSERT_FALSE(decode(uint32Words, 2U, TEXT, Full, Big).ok);
    TEST_ASSERT_FALSE(decode(nullptr, 0U, UINT16, Full, Big).ok);
    const uint16_t extra[] = {0x0001U, 0xFFFFU, 0xFFFFU};
    TEST_ASSERT_TRUE(decode(extra, 3U, UINT16, Full, Big).rawUnsigned == 1U);
    TEST_ASSERT_TRUE(decode(extra, 3U, UINT32, Full, Big).rawUnsigned == 0x0001FFFFU);
}

void test_modbus_codec_encode_round_trips(void) {
    using namespace ModbusValueCodec;
    constexpr auto Big = ModbusWordOrder::Big;
    constexpr auto Little = ModbusWordOrder::Little;
    constexpr auto Full = RegisterSlice::Full;
    EncodedWords encoded;

    TEST_ASSERT_TRUE(encodeSigned(-200, INT16, Big, encoded));
    TEST_ASSERT_EQUAL_UINT8(1U, encoded.count);
    TEST_ASSERT_EQUAL_UINT16(0xFF38U, encoded.words[0]);
    TEST_ASSERT_TRUE(encodeSigned(-32768, INT16, Big, encoded));
    TEST_ASSERT_EQUAL_UINT16(0x8000U, encoded.words[0]);
    TEST_ASSERT_TRUE(encodeSigned(32767, INT16, Little, encoded));
    TEST_ASSERT_EQUAL_UINT16(0x7FFFU, encoded.words[0]);
    TEST_ASSERT_FALSE(encodeSigned(-32769, INT16, Big, encoded));
    TEST_ASSERT_FALSE(encodeSigned(32768, INT16, Big, encoded));
    TEST_ASSERT_FALSE(encodeSigned(1, UINT16, Big, encoded));
    TEST_ASSERT_FALSE(encodeSigned(1, FLOAT32, Big, encoded));
    TEST_ASSERT_FALSE(encodeSigned(1, TEXT, Big, encoded));

    TEST_ASSERT_TRUE(encodeUnsigned(65535U, UINT16, Big, encoded));
    TEST_ASSERT_EQUAL_UINT8(1U, encoded.count);
    TEST_ASSERT_EQUAL_UINT16(0xFFFFU, encoded.words[0]);
    TEST_ASSERT_FALSE(encodeUnsigned(65536U, UINT16, Big, encoded));
    TEST_ASSERT_FALSE(encodeUnsigned(1U, INT16, Big, encoded));

    TEST_ASSERT_TRUE(encodeSigned(std::numeric_limits<int32_t>::min(), INT32, Big, encoded));
    TEST_ASSERT_EQUAL_UINT8(2U, encoded.count);
    TEST_ASSERT_EQUAL_UINT16(0x8000U, encoded.words[0]);
    TEST_ASSERT_EQUAL_UINT16(0x0000U, encoded.words[1]);
    TEST_ASSERT_TRUE(encodeSigned(std::numeric_limits<int32_t>::min(), INT32, Little, encoded));
    TEST_ASSERT_EQUAL_UINT16(0x0000U, encoded.words[0]);
    TEST_ASSERT_EQUAL_UINT16(0x8000U, encoded.words[1]);
    TEST_ASSERT_TRUE(encodeSigned(-1, INT32, Big, encoded));
    TEST_ASSERT_EQUAL_UINT16(0xFFFFU, encoded.words[0]);
    TEST_ASSERT_EQUAL_UINT16(0xFFFFU, encoded.words[1]);
    TEST_ASSERT_FALSE(encodeSigned(static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1,
                                   INT32, Big, encoded));
    TEST_ASSERT_FALSE(encodeSigned(static_cast<int64_t>(std::numeric_limits<int32_t>::min()) - 1,
                                   INT32, Big, encoded));
    TEST_ASSERT_TRUE(encodeUnsigned(4294967295ULL, UINT32, Big, encoded));
    TEST_ASSERT_EQUAL_UINT16(0xFFFFU, encoded.words[0]);
    TEST_ASSERT_EQUAL_UINT16(0xFFFFU, encoded.words[1]);
    TEST_ASSERT_FALSE(encodeUnsigned(4294967296ULL, UINT32, Big, encoded));
    TEST_ASSERT_TRUE(encodeUnsigned(65536U, UINT32, Little, encoded));
    TEST_ASSERT_EQUAL_UINT16(0x0000U, encoded.words[0]);
    TEST_ASSERT_EQUAL_UINT16(0x0001U, encoded.words[1]);

    TEST_ASSERT_TRUE(encodeSigned(std::numeric_limits<int64_t>::min(), INT64, Big, encoded));
    TEST_ASSERT_EQUAL_UINT8(4U, encoded.count);
    TEST_ASSERT_EQUAL_UINT16(0x8000U, encoded.words[0]);
    TEST_ASSERT_EQUAL_UINT16(0x0000U, encoded.words[3]);
    TEST_ASSERT_TRUE(encodeSigned(-1, INT64, Little, encoded));
    for (size_t index = 0U; index < 4U; ++index) TEST_ASSERT_EQUAL_UINT16(0xFFFFU, encoded.words[index]);
    TEST_ASSERT_TRUE(encodeUnsigned(std::numeric_limits<uint64_t>::max(), UINT64, Big, encoded));
    for (size_t index = 0U; index < 4U; ++index) TEST_ASSERT_EQUAL_UINT16(0xFFFFU, encoded.words[index]);
    TEST_ASSERT_TRUE(encodeUnsigned(0x0001000200030004ULL, UINT64, Little, encoded));
    TEST_ASSERT_EQUAL_UINT16(0x0004U, encoded.words[0]);
    TEST_ASSERT_EQUAL_UINT16(0x0003U, encoded.words[1]);
    TEST_ASSERT_EQUAL_UINT16(0x0002U, encoded.words[2]);
    TEST_ASSERT_EQUAL_UINT16(0x0001U, encoded.words[3]);

    TEST_ASSERT_TRUE(encodeFloat(123.456F, Big, encoded));
    TEST_ASSERT_EQUAL_UINT8(2U, encoded.count);
    TEST_ASSERT_EQUAL_UINT16(0x42F6U, encoded.words[0]);
    TEST_ASSERT_EQUAL_UINT16(0xE979U, encoded.words[1]);
    TEST_ASSERT_TRUE(encodeFloat(123.456F, Little, encoded));
    TEST_ASSERT_EQUAL_UINT16(0xE979U, encoded.words[0]);
    TEST_ASSERT_EQUAL_UINT16(0x42F6U, encoded.words[1]);

    // Every encoder output decodes back to the same raw value in both orders.
    const ModbusDataType signedTypes[] = {INT16, INT32, INT64};
    const int64_t signedSamples[] = {0, 1, -1, 12345, -12345, 32767, -32768};
    const ModbusDataType unsignedTypes[] = {UINT16, UINT32, UINT64};
    const uint64_t unsignedSamples[] = {0U, 1U, 255U, 65535U};
    const ModbusWordOrder orders[] = {Big, Little};
    for (const auto order : orders) {
        for (const auto type : signedTypes) {
            for (const int64_t sample : signedSamples) {
                TEST_ASSERT_TRUE(encodeSigned(sample, type, order, encoded));
                TEST_ASSERT_EQUAL_UINT8(wordWidth(type), encoded.count);
                const DecodedValue decoded = decode(encoded.words.data(), encoded.count, type, Full, order);
                TEST_ASSERT_TRUE(decoded.ok);
                TEST_ASSERT_TRUE(decoded.rawSigned == sample);
            }
        }
        for (const auto type : unsignedTypes) {
            for (const uint64_t sample : unsignedSamples) {
                TEST_ASSERT_TRUE(encodeUnsigned(sample, type, order, encoded));
                const DecodedValue decoded = decode(encoded.words.data(), encoded.count, type, Full, order);
                TEST_ASSERT_TRUE(decoded.ok);
                TEST_ASSERT_TRUE(decoded.rawUnsigned == sample);
            }
        }
        TEST_ASSERT_TRUE(encodeFloat(-2.5F, order, encoded));
        const DecodedValue decoded = decode(encoded.words.data(), encoded.count, FLOAT32, Full, order);
        TEST_ASSERT_TRUE(decoded.ok);
        TEST_ASSERT_TRUE(decoded.rawFloat == -2.5F);
    }
}

void test_modbus_codec_formats_decimals_from_scale(void) {
    using namespace ModbusValueCodec;
    struct DecimalsCase {
        float scale;
        int decimals;
    };
    const DecimalsCase decimalsCases[] = {
        {0.01F, 2}, {1.0F, 0}, {0.001F, 3}, {0.5F, 1}, {-0.1F, 1}, {2.5F, 1},
        {100.0F, 0}, {1e-5F, 5}, {0.123456789F, 6}, {1e-7F, 6}, {0.0F, 0},
        {10.5F, 1}, {0.25F, 2}, {1.0e6F, 0},
    };
    for (const auto &testCase : decimalsCases) {
        TEST_ASSERT_EQUAL_INT(testCase.decimals, decimalsForScale(testCase.scale));
    }

    char text[64];
    DecodedValue value;
    value.ok = true;
    const auto format = [&](const double raw, const ModbusDataType type, const float scale) {
        value.raw = raw;
        return formatEngineering(engineeringValue(value, scale), type, scale, text, sizeof text);
    };

    TEST_ASSERT_TRUE(format(1234.0, INT16, 1.0F).finite);
    TEST_ASSERT_EQUAL_STRING("1234", text);
    TEST_ASSERT_EQUAL_UINT(4U, format(1234.0, INT16, 1.0F).length);
    format(3.0, UINT16, 0.5F);
    TEST_ASSERT_EQUAL_STRING("1.5", text);
    format(0.0, INT16, -0.1F);
    TEST_ASSERT_EQUAL_STRING("0.0", text);
    format(-1.0, INT16, 0.001F);
    TEST_ASSERT_EQUAL_STRING("-0.001", text);
    format(-1.0, INT16, 1e-7F);
    TEST_ASSERT_EQUAL_STRING("0.000000", text);
    format(65535.0, UINT16, 0.1F);
    TEST_ASSERT_EQUAL_STRING("6553.5", text);
    format(1.0, UINT16, 1.0F);
    TEST_ASSERT_EQUAL_STRING("1", text);
    format(-100000.0, INT32, 1.0F);
    TEST_ASSERT_EQUAL_STRING("-100000", text);
    // The double path rounds above 2^53; formatDecoded keeps the exact digits.
    format(static_cast<double>(std::numeric_limits<int64_t>::max()), INT64, 1.0F);
    TEST_ASSERT_EQUAL_STRING("9223372036854775808", text);

    format(3.14F, FLOAT32, 1.0F);
    TEST_ASSERT_EQUAL_STRING("3.14", text);
    format(1e10, FLOAT32, 1.0F);
    TEST_ASSERT_EQUAL_STRING("1e+10", text);
    format(-100.0, FLOAT32, 1.0F);
    TEST_ASSERT_EQUAL_STRING("-100", text);
    format(0.1F, FLOAT32, 1.0F);
    TEST_ASSERT_EQUAL_STRING("0.1", text);
    format(2.5F, FLOAT32, 0.5F);
    TEST_ASSERT_EQUAL_STRING("1.25", text);

    Formatted formatted = format(NAN, FLOAT32, 1.0F);
    TEST_ASSERT_FALSE(formatted.finite);
    TEST_ASSERT_EQUAL_STRING("nan", text);
    formatted = format(INFINITY, FLOAT32, 1.0F);
    TEST_ASSERT_FALSE(formatted.finite);
    TEST_ASSERT_EQUAL_STRING("inf", text);
    formatted = format(-INFINITY, FLOAT32, 1.0F);
    TEST_ASSERT_FALSE(formatted.finite);
    TEST_ASSERT_EQUAL_STRING("-inf", text);

    char tiny[4];
    formatted = formatEngineering(123.456, FLOAT32, 1.0F, tiny, sizeof tiny);
    TEST_ASSERT_FALSE(formatted.finite);
    TEST_ASSERT_EQUAL_UINT(0U, formatted.length);
    TEST_ASSERT_EQUAL_STRING("", tiny);
    TEST_ASSERT_EQUAL_UINT(0U, formatEngineering(1.0, INT16, 1.0F, nullptr, 0U).length);
}

void test_modbus_codec_formats_wide_integers_exactly(void) {
    using namespace ModbusValueCodec;
    constexpr auto Big = ModbusWordOrder::Big;
    constexpr auto Full = RegisterSlice::Full;
    char text[64];
    char expected[64];

    // A uint64 serial number above 2^53 keeps every digit at integral scales
    // instead of rounding through a double (…321 would become …336).
    const uint16_t serial[] = {0x1234U, 0x5678U, 0x9ABCU, 0xDEF1U};
    DecodedValue decoded = decode(serial, 4U, UINT64, Full, Big);
    TEST_ASSERT_TRUE(decoded.ok);
    TEST_ASSERT_TRUE(decoded.rawUnsigned == 0x123456789ABCDEF1ULL);
    Formatted formatted = formatDecoded(decoded, UINT64, 1.0F, text, sizeof text);
    TEST_ASSERT_TRUE(formatted.finite);
    TEST_ASSERT_EQUAL_UINT(19U, formatted.length);
    TEST_ASSERT_EQUAL_STRING("1311768467463790321", text);
    formatDecoded(decoded, UINT64, -1.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("-1311768467463790321", text);
    formatDecoded(decoded, UINT64, 10.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("13117684674637903210", text);
    // A product that does not fit 64 bits, or a fractional scale, rounds
    // through a double as documented.
    formatDecoded(decoded, UINT64, 100.0F, text, sizeof text);
    std::snprintf(expected, sizeof expected, "%.0f", static_cast<double>(decoded.rawUnsigned) * 100.0);
    TEST_ASSERT_EQUAL_STRING(expected, text);
    formatDecoded(decoded, UINT64, 0.5F, text, sizeof text);
    std::snprintf(expected, sizeof expected, "%.1f", static_cast<double>(decoded.rawUnsigned) * 0.5);
    TEST_ASSERT_EQUAL_STRING(expected, text);

    const uint16_t int64Max[] = {0x7FFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU};
    decoded = decode(int64Max, 4U, INT64, Full, Big);
    formatDecoded(decoded, INT64, 1.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("9223372036854775807", text);
    formatDecoded(decoded, INT64, -1.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("-9223372036854775807", text);
    const uint16_t int64Min[] = {0x8000U, 0U, 0U, 0U};
    decoded = decode(int64Min, 4U, INT64, Full, Big);
    formatDecoded(decoded, INT64, 1.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("-9223372036854775808", text);
    formatDecoded(decoded, INT64, -1.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("9223372036854775808", text);
    formatDecoded(decoded, INT64, 2.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("-18446744073709551616", text);  // overflow: double path
    const uint16_t uint64Max[] = {0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU};
    decoded = decode(uint64Max, 4U, UINT64, Full, Big);
    formatDecoded(decoded, UINT64, 1.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("18446744073709551615", text);
    formatDecoded(decoded, UINT64, 1e6F, text, sizeof text);
    std::snprintf(expected, sizeof expected, "%.0f", static_cast<double>(decoded.rawUnsigned) * 1e6);
    TEST_ASSERT_EQUAL_STRING(expected, text);

    // Narrower integers take the same path and print as before.
    const uint16_t nilan[] = {0xFF38U};
    decoded = decode(nilan, 1U, INT16, Full, Big);
    formatDecoded(decoded, INT16, 1.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("-200", text);
    formatDecoded(decoded, INT16, -1.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("200", text);
    formatDecoded(decoded, INT16, 0.01F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("-2.00", text);
    formatDecoded(decoded, INT16, 100.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("-20000", text);
    const uint16_t zero[] = {0U};
    decoded = decode(zero, 1U, UINT16, Full, Big);
    formatDecoded(decoded, UINT16, -1.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("0", text);  // never "-0"
    decoded = decode(zero, 1U, INT16, Full, Big);
    formatDecoded(decoded, INT16, -5.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("0", text);

    char tiny[8];
    formatted = formatDecoded(decode(serial, 4U, UINT64, Full, Big), UINT64, 1.0F, tiny, sizeof tiny);
    TEST_ASSERT_EQUAL_UINT(0U, formatted.length);
    TEST_ASSERT_FALSE(formatted.finite);
    TEST_ASSERT_EQUAL_UINT(0U, formatDecoded(decoded, INT16, 1.0F, nullptr, 0U).length);
}

void test_modbus_codec_formats_float32_with_shortest_round_trip(void) {
    using namespace ModbusValueCodec;
    char text[64];
    DecodedValue value;
    value.ok = true;
    const auto format = [&](const float raw, const float scale) {
        value.rawFloat = raw;
        value.raw = static_cast<double>(raw);
        return formatDecoded(value, FLOAT32, scale, text, sizeof text);
    };

    // 2^24 - 1 is exact in float32; "%.7g" would print 1.677722e+07.
    format(16777215.0F, 1.0F);
    TEST_ASSERT_EQUAL_STRING("16777215", text);
    TEST_ASSERT_TRUE(std::strtof(text, nullptr) == 16777215.0F);
    // The shortest text that reads back as the same float32 is preferred.
    format(1234567.75F, 1.0F);
    TEST_ASSERT_EQUAL_STRING("1234567.8", text);
    TEST_ASSERT_TRUE(std::strtof(text, nullptr) == 1234567.75F);
    format(0.1F, 1.0F);
    TEST_ASSERT_EQUAL_STRING("0.1", text);
    format(3.14F, 1.0F);
    TEST_ASSERT_EQUAL_STRING("3.14", text);
    format(-100.0F, 1.0F);
    TEST_ASSERT_EQUAL_STRING("-100", text);
    format(1e10F, 1.0F);
    TEST_ASSERT_EQUAL_STRING("1e+10", text);
    format(123456789.0F, 1.0F);
    TEST_ASSERT_EQUAL_STRING("1.2345679e+08", text);
    TEST_ASSERT_TRUE(std::strtof(text, nullptr) == 123456789.0F);
    format(2.5F, 0.5F);
    TEST_ASSERT_EQUAL_STRING("1.25", text);
    format(0.0F, 1.0F);
    TEST_ASSERT_EQUAL_STRING("0", text);
    format(-0.0F, 1.0F);
    TEST_ASSERT_EQUAL_STRING("0", text);
    // raw * scale beyond float32 prints the double product with 9 digits.
    format(3e38F, 10.0F);
    char expected[64];
    std::snprintf(expected, sizeof expected, "%.9g", static_cast<double>(3e38F) * 10.0);
    TEST_ASSERT_EQUAL_STRING(expected, text);
    TEST_ASSERT_EQUAL_STRING("e+39", text + std::strlen(text) - 4U);
    Formatted formatted = format(NAN, 1.0F);
    TEST_ASSERT_FALSE(formatted.finite);
    TEST_ASSERT_EQUAL_STRING("nan", text);
}

void test_modbus_codec_format_reading_matches_publication(void) {
    using namespace ModbusValueCodec;
    constexpr auto Big = ModbusWordOrder::Big;
    constexpr auto Little = ModbusWordOrder::Little;
    constexpr auto Full = RegisterSlice::Full;
    char text[64];

    const uint16_t nilan[] = {0xFF38U};
    ReadingText reading = formatReading(nilan, 1U, false, INT16, Full, Big, 0.01F, text, sizeof text);
    TEST_ASSERT_TRUE(reading.ok);
    TEST_ASSERT_TRUE(reading.finite);
    TEST_ASSERT_EQUAL_UINT8(1U, reading.width);
    TEST_ASSERT_EQUAL_UINT(5U, reading.length);
    TEST_ASSERT_EQUAL_STRING("-2.00", text);

    // Coils and discrete inputs are packed bits: decoded as uint16 whatever
    // the data type says.
    const uint16_t coils[] = {0x0005U};
    reading = formatReading(coils, 1U, true, INT32, Full, Big, 1.0F, text, sizeof text);
    TEST_ASSERT_TRUE(reading.ok);
    TEST_ASSERT_EQUAL_UINT8(1U, reading.width);
    TEST_ASSERT_EQUAL_STRING("5", text);
    reading = formatReading(coils, 1U, true, TEXT, Full, Big, 1.0F, text, sizeof text);
    TEST_ASSERT_TRUE(reading.ok);
    TEST_ASSERT_EQUAL_STRING("5", text);

    // Too few registers for the type: nothing to publish, width says how many.
    reading = formatReading(nilan, 1U, false, INT32, Full, Big, 1.0F, text, sizeof text);
    TEST_ASSERT_FALSE(reading.ok);
    TEST_ASSERT_EQUAL_UINT8(2U, reading.width);
    TEST_ASSERT_EQUAL_STRING("", text);
    reading = formatReading(nilan, 1U, false, TEXT, Full, Big, 1.0F, text, sizeof text);
    TEST_ASSERT_FALSE(reading.ok);
    TEST_ASSERT_EQUAL_UINT8(0U, reading.width);

    const uint16_t pi[] = {0xF5C3U, 0x4048U};
    reading = formatReading(pi, 2U, false, FLOAT32, Full, Little, 1.0F, text, sizeof text);
    TEST_ASSERT_TRUE(reading.ok);
    TEST_ASSERT_EQUAL_STRING("3.14", text);
    const uint16_t notANumber[] = {0x7FC0U, 0U};
    reading = formatReading(notANumber, 2U, false, FLOAT32, Full, Big, 1.0F, text, sizeof text);
    TEST_ASSERT_TRUE(reading.ok);
    TEST_ASSERT_FALSE(reading.finite);
    TEST_ASSERT_EQUAL_STRING("nan", text);

    const uint16_t serial[] = {0x1234U, 0x5678U, 0x9ABCU, 0xDEF1U};
    reading = formatReading(serial, 4U, false, UINT64, Full, Big, 1.0F, text, sizeof text);
    TEST_ASSERT_TRUE(reading.ok);
    TEST_ASSERT_EQUAL_STRING("1311768467463790321", text);

    const uint16_t sliced[] = {0x1280U};
    reading = formatReading(sliced, 1U, false, INT16, RegisterSlice::LowByte, Big, 1.0F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("-128", text);
    reading = formatReading(sliced, 1U, false, UINT16, RegisterSlice::HighByte, Big, 0.5F, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("9.0", text);

    reading = formatReading(nullptr, 0U, false, UINT16, Full, Big, 1.0F, text, sizeof text);
    TEST_ASSERT_FALSE(reading.ok);
    TEST_ASSERT_EQUAL_STRING("", text);
}

void test_modbus_codec_parses_word_lists(void) {
    constexpr size_t kCapacity = 32U;
    uint16_t words[kCapacity + 1U] = {};
    uint8_t count = 0U;
    TEST_ASSERT_TRUE(ModbusValueCodec::parseWordList("2026,10, 17 ,0x5,0X0e,0,65535", words, kCapacity, count));
    TEST_ASSERT_EQUAL_UINT8(7U, count);
    const uint16_t expected[7] = {2026U, 10U, 17U, 5U, 14U, 0U, 65535U};
    TEST_ASSERT_EQUAL_HEX16_ARRAY(expected, words, 7U);
    TEST_ASSERT_TRUE(ModbusValueCodec::parseWordList("0xffff", words, kCapacity, count));
    TEST_ASSERT_EQUAL_UINT8(1U, count);
    TEST_ASSERT_EQUAL_HEX16(0xFFFFU, words[0]);

    // Rejected: empty lists and entries, other text, values out of range.
    const char *const rejected[] = {"", " ", "1,", ",1", "1,,2", "1;2", "-1", "+1", "1.5", "0x",
                                    "0x10000", "65536", "1 2", "abc"};
    for (const char *text : rejected) {
        count = 9U;
        TEST_ASSERT_FALSE_MESSAGE(ModbusValueCodec::parseWordList(text, words, kCapacity, count), text);
        TEST_ASSERT_EQUAL_UINT8(0U, count);
    }
    // The capacity is the only limit on the count.
    char list[160] = "0";
    for (size_t i = 1U; i <= kCapacity; ++i) std::strcat(list, ",0");
    TEST_ASSERT_FALSE(ModbusValueCodec::parseWordList(list, words, kCapacity, count));
    TEST_ASSERT_EQUAL_UINT8(0U, count);
    TEST_ASSERT_TRUE(ModbusValueCodec::parseWordList(list, words, kCapacity + 1U, count));
    TEST_ASSERT_EQUAL_UINT8(kCapacity + 1U, count);
    list[std::strlen(list) - 2U] = '\0';
    TEST_ASSERT_TRUE(ModbusValueCodec::parseWordList(list, words, kCapacity, count));
    TEST_ASSERT_EQUAL_UINT8(kCapacity, count);
    TEST_ASSERT_FALSE(ModbusValueCodec::parseWordList("1,2,3", words, 2U, count));
    TEST_ASSERT_FALSE(ModbusValueCodec::parseWordList("1", words, 0U, count));
    TEST_ASSERT_FALSE(ModbusValueCodec::parseWordList(nullptr, words, kCapacity, count));
    TEST_ASSERT_FALSE(ModbusValueCodec::parseWordList("1", nullptr, kCapacity, count));
}

void test_ota_lifecycle_rejects_duplicates_and_retains_terminal_failure(void) {
    OtaProgressCore::Lifecycle lifecycle;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(OtaProgressCore::Lifecycle::Admission::Accepted),
        static_cast<int>(lifecycle.reserve("8.1.0", true, true, false)));
    const uint32_t operation = lifecycle.operationId();
    TEST_ASSERT_NOT_EQUAL(0U, operation);
    TEST_ASSERT_FALSE(lifecycle.markAwaitingUpload(operation + 1U));
    TEST_ASSERT_FALSE(lifecycle.beginVerification(operation, OtaProgressCore::Component::Firmware));
    TEST_ASSERT_FALSE(lifecycle.markResuming(operation));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(OtaProgressCore::Lifecycle::Admission::Busy),
        static_cast<int>(lifecycle.reserve("8.1.1", true, true, false)));
    TEST_ASSERT_TRUE(lifecycle.failureDetected(operation, "quiesce_timeout"));
    TEST_ASSERT_TRUE(lifecycle.markResuming(operation));
    TEST_ASSERT_TRUE(lifecycle.markFailed(operation, "quiesce_timeout"));
    TEST_ASSERT_FALSE(lifecycle.active());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(OtaProgressCore::State::Failed),
                          static_cast<int>(lifecycle.snapshot().state));
    TEST_ASSERT_EQUAL_STRING("quiesce_timeout", lifecycle.snapshot().errorCode);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(OtaProgressCore::Lifecycle::Admission::Accepted),
        static_cast<int>(lifecycle.reserve("8.1.1", true, false, true)));
    TEST_ASSERT_NOT_EQUAL(operation, lifecycle.operationId());
}

void test_ota_quiescence_deadlines_are_independent_and_rollover_safe(void) {
    using OtaQuiescenceCore::Result;
    const uint32_t startedAt = UINT32_MAX - 100U;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Pending), static_cast<int>(
        OtaQuiescenceCore::evaluate(startedAt, 899U, false, false, false,
                                    1500U, 2500U, 6000U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::MqttTimeout), static_cast<int>(
        OtaQuiescenceCore::evaluate(startedAt, 1400U, false, false, false,
                                    1500U, 2500U, 6000U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::ModbusTimeout), static_cast<int>(
        OtaQuiescenceCore::evaluate(startedAt, 2400U, true, false, false,
                                    1500U, 2500U, 6000U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::HttpTimeout), static_cast<int>(
        OtaQuiescenceCore::evaluate(startedAt, 5900U, true, true, false,
                                    1500U, 2500U, 6000U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Result::Ready), static_cast<int>(
        OtaQuiescenceCore::evaluate(startedAt, 5900U, true, true, true,
                                    1500U, 2500U, 6000U)));
}

void test_ota_progress_calculates_component_and_overall_without_overflow(void) {
    OtaProgressCore::Lifecycle lifecycle;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(OtaProgressCore::Lifecycle::Admission::Accepted),
        static_cast<int>(lifecycle.reserve("8.2.0", true, true, false)));
    const uint32_t operation = lifecycle.operationId();
    TEST_ASSERT_TRUE(lifecycle.beginComponent(operation, OtaProgressCore::Component::Firmware,
                                              UINT32_MAX));
    TEST_ASSERT_TRUE(lifecycle.updateBytes(operation, UINT32_MAX - 1U));
    TEST_ASSERT_EQUAL_UINT8(99U, lifecycle.snapshot().componentPercent);
    TEST_ASSERT_FALSE(lifecycle.snapshot().overallPercentKnown);
    TEST_ASSERT_TRUE(lifecycle.beginVerification(operation, OtaProgressCore::Component::Firmware));
    TEST_ASSERT_TRUE(lifecycle.completeComponent(operation, OtaProgressCore::Component::Firmware));
    TEST_ASSERT_TRUE(lifecycle.beginComponent(operation, OtaProgressCore::Component::Filesystem, 100U));
    TEST_ASSERT_TRUE(lifecycle.updateBytes(operation, 50U));
    TEST_ASSERT_TRUE(lifecycle.snapshot().overallPercentKnown);
    TEST_ASSERT_EQUAL_UINT8(99U, lifecycle.snapshot().overallPercent);
    TEST_ASSERT_TRUE(lifecycle.updateBytes(operation, 1000U));
    TEST_ASSERT_EQUAL_UINT8(100U, lifecycle.snapshot().componentPercent);
    TEST_ASSERT_EQUAL_UINT32(100U, lifecycle.snapshot().receivedBytes);
}

void test_ota_manual_progress_and_success_transition(void) {
    OtaProgressCore::Lifecycle lifecycle;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(OtaProgressCore::Lifecycle::Admission::Accepted),
        static_cast<int>(lifecycle.reserve("manual", false, true, true)));
    const uint32_t operation = lifecycle.operationId();
    TEST_ASSERT_TRUE(lifecycle.markAwaitingUpload(operation));
    TEST_ASSERT_TRUE(lifecycle.beginComponent(operation, OtaProgressCore::Component::Filesystem, 400U));
    TEST_ASSERT_TRUE(lifecycle.updateBytes(operation, 100U));
    TEST_ASSERT_TRUE(lifecycle.snapshot().overallPercentKnown);
    TEST_ASSERT_EQUAL_UINT8(25U, lifecycle.snapshot().componentPercent);
    TEST_ASSERT_EQUAL_UINT8(25U, lifecycle.snapshot().overallPercent);
    TEST_ASSERT_TRUE(lifecycle.beginVerification(operation, OtaProgressCore::Component::Filesystem));
    TEST_ASSERT_TRUE(lifecycle.completeComponent(operation, OtaProgressCore::Component::Filesystem));
    TEST_ASSERT_TRUE(lifecycle.markRebooting(operation));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(OtaProgressCore::State::Rebooting),
                          static_cast<int>(lifecycle.snapshot().state));
}

void test_ota_signed_single_component_reports_overall_progress(void) {
    OtaProgressCore::Lifecycle lifecycle;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(OtaProgressCore::Lifecycle::Admission::Accepted),
        static_cast<int>(lifecycle.reserve("8.2.1", true, false, false)));
    const uint32_t operation = lifecycle.operationId();
    TEST_ASSERT_TRUE(lifecycle.beginComponent(
        operation, OtaProgressCore::Component::Firmware, 400U));
    TEST_ASSERT_TRUE(lifecycle.updateBytes(operation, 100U));
    TEST_ASSERT_TRUE(lifecycle.snapshot().overallPercentKnown);
    TEST_ASSERT_EQUAL_UINT8(25U, lifecycle.snapshot().overallPercent);
}

void test_ota_heap_floor_requires_free_and_contiguous_headroom(void) {
    TEST_ASSERT_TRUE(OtaProgressCore::meetsHeapFloor(100U, 50U, 100U, 50U));
    TEST_ASSERT_FALSE(OtaProgressCore::meetsHeapFloor(99U, 50U, 100U, 50U));
    TEST_ASSERT_FALSE(OtaProgressCore::meetsHeapFloor(100U, 49U, 100U, 50U));
}

// Runtime mirror of the static_asserts in Config.h. The asserts permit
// equality for -D overrides; the shipped defaults are expected to be strictly
// ordered because the flash-boundary check runs after TLS has been paid for.
void test_ota_flash_boundary_floor_sits_below_quiescence_floor(void) {
    TEST_ASSERT_TRUE(OTA_FLASH_MIN_INTERNAL_FREE_HEAP < OTA_MIN_INTERNAL_FREE_HEAP);
    TEST_ASSERT_TRUE(OTA_FLASH_MIN_INTERNAL_LARGEST_BLOCK < OTA_MIN_INTERNAL_LARGEST_BLOCK);
}

// Regression anchor from a serial capture on the custom board: quiescence
// measured 122,200 B free / 40,948 B largest, and the post-TLS reading at the
// flash boundary was 76,248 B (second attempt 77,412 B) with the same largest
// block. One shared 96 KiB floor refused every HTTPS download on this
// hardware.
void test_ota_two_stage_admission_matches_observed_tls_cost(void) {
    TEST_ASSERT_TRUE(OtaProgressCore::meetsHeapFloor(
        122200U, 40948U, OTA_MIN_INTERNAL_FREE_HEAP, OTA_MIN_INTERNAL_LARGEST_BLOCK));
    TEST_ASSERT_TRUE(OtaProgressCore::meetsHeapFloor(
        76248U, 40948U, OTA_FLASH_MIN_INTERNAL_FREE_HEAP, OTA_FLASH_MIN_INTERNAL_LARGEST_BLOCK));
    TEST_ASSERT_TRUE(OtaProgressCore::meetsHeapFloor(
        77412U, 40948U, OTA_FLASH_MIN_INTERNAL_FREE_HEAP, OTA_FLASH_MIN_INTERNAL_LARGEST_BLOCK));
    // The former single-stage floor refused the post-TLS reading.
    TEST_ASSERT_FALSE(OtaProgressCore::meetsHeapFloor(
        76248U, 40948U, OTA_MIN_INTERNAL_FREE_HEAP, OTA_MIN_INTERNAL_LARGEST_BLOCK));
    // The flash-boundary floor still refuses genuinely exhausted memory.
    TEST_ASSERT_FALSE(OtaProgressCore::meetsHeapFloor(
        40000U, 40948U, OTA_FLASH_MIN_INTERNAL_FREE_HEAP, OTA_FLASH_MIN_INTERNAL_LARGEST_BLOCK));
    TEST_ASSERT_FALSE(OtaProgressCore::meetsHeapFloor(
        76248U, 12000U, OTA_FLASH_MIN_INTERNAL_FREE_HEAP, OTA_FLASH_MIN_INTERNAL_LARGEST_BLOCK));
}

void test_ota_manual_transfer_timeout_covers_start_idle_and_rollover(void) {
    using OtaProgressCore::State;
    TEST_ASSERT_FALSE(OtaProgressCore::manualTransferTimedOut(
        State::AwaitingUpload, false, 30000U, 0U, 30000U));
    TEST_ASSERT_FALSE(OtaProgressCore::manualTransferTimedOut(
        State::AwaitingUpload, true, 29999U, 0U, 30000U));
    TEST_ASSERT_TRUE(OtaProgressCore::manualTransferTimedOut(
        State::AwaitingUpload, true, 30000U, 0U, 30000U));
    TEST_ASSERT_TRUE(OtaProgressCore::manualTransferTimedOut(
        State::DownloadingWritingFirmware, true, 20000U, UINT32_MAX - 9999U, 30000U));
    TEST_ASSERT_TRUE(OtaProgressCore::manualTransferTimedOut(
        State::DownloadingWritingFilesystem, true, 30000U, 0U, 30000U));
    TEST_ASSERT_FALSE(OtaProgressCore::manualTransferTimedOut(
        State::VerifyingFirmware, true, 30000U, 0U, 30000U));
}

void test_ota_sessions_enforce_capacity_expiry_revocation_and_hex_tokens(void) {
    OtaSessionCore::Store<2U> sessions(900000U, 300000U);
    uint8_t first[OtaSessionCore::TOKEN_BYTES]{};
    uint8_t second[OtaSessionCore::TOKEN_BYTES]{};
    uint8_t third[OtaSessionCore::TOKEN_BYTES]{};
    for (size_t i = 0U; i < OtaSessionCore::TOKEN_BYTES; ++i) {
        first[i] = static_cast<uint8_t>(i);
        second[i] = static_cast<uint8_t>(i + 1U);
        third[i] = static_cast<uint8_t>(i + 2U);
    }
    TEST_ASSERT_TRUE(sessions.create(first, UINT32_MAX - 1000U));
    TEST_ASSERT_TRUE(sessions.create(second, UINT32_MAX - 1000U));
    TEST_ASSERT_FALSE(sessions.create(third, UINT32_MAX - 1000U));
    TEST_ASSERT_TRUE(sessions.authorize(first, 500U));
    TEST_ASSERT_FALSE(sessions.authorize(third, 500U));

    char encoded[OtaSessionCore::TOKEN_BYTES * 2U + 1U]{};
    uint8_t decoded[OtaSessionCore::TOKEN_BYTES]{};
    OtaSessionCore::encodeHexToken(first, encoded);
    TEST_ASSERT_TRUE(OtaSessionCore::decodeHexToken(encoded, std::strlen(encoded), decoded));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, decoded, OtaSessionCore::TOKEN_BYTES);
    TEST_ASSERT_FALSE(OtaSessionCore::decodeHexToken("abcd", 4U, decoded));

    sessions.expire(300500U);
    TEST_ASSERT_EQUAL_UINT(0U, sessions.activeCount());
    TEST_ASSERT_TRUE(sessions.create(third, 300500U));
    sessions.revokeAll();
    TEST_ASSERT_FALSE(sessions.authorize(third, 300501U));
}

void test_ota_progress_publisher_coalesces_and_applies_queue_heap_guards(void) {
    OtaProgressCore::Publisher publisher;
    OtaProgressCore::Snapshot snapshot;
    snapshot.operationId = 1U;
    snapshot.sequence = 1U;
    snapshot.state = OtaProgressCore::State::Quiescing;
    TEST_ASSERT_FALSE(publisher.shouldPublish(snapshot, 1000U, 0U, 0U,
                                              100000U, 50000U, 80000U, 24000U));
    TEST_ASSERT_FALSE(publisher.shouldPublish(snapshot, 1000U, 1U, 1U,
                                              100000U, 50000U, 80000U, 24000U));
    TEST_ASSERT_FALSE(publisher.shouldPublish(snapshot, 1000U, 1U, 0U,
                                              70000U, 50000U, 80000U, 24000U));
    TEST_ASSERT_TRUE(publisher.shouldPublish(snapshot, 1000U, 1U, 0U,
                                             100000U, 50000U, 80000U, 24000U));
    TEST_ASSERT_FALSE(publisher.shouldPublish(snapshot, 2000U, 1U, 0U,
                                              100000U, 50000U, 80000U, 24000U));
    snapshot.sequence = 2U;
    snapshot.componentPercent = 1U;
    TEST_ASSERT_FALSE(publisher.shouldPublish(snapshot, 1050U, 1U, 0U,
                                              100000U, 50000U, 80000U, 24000U));
    TEST_ASSERT_TRUE(publisher.shouldPublish(snapshot, 1100U, 1U, 0U,
                                             100000U, 50000U, 80000U, 24000U));
    snapshot.sequence = 3U;
    TEST_ASSERT_TRUE(publisher.shouldPublish(snapshot, 1101U, 1U, 0U,
                                             100000U, 50000U, 80000U, 24000U, true));
}

void test_ota_admission_gate_waits_for_existing_ui_lease_and_restores_selectively(void) {
    OtaAdmissionGateCore::Gate gate;
    TEST_ASSERT_TRUE(gate.tryAcquireUiLease());
    gate.close();
    TEST_ASSERT_FALSE(gate.ordinaryMutationOpen());
    TEST_ASSERT_FALSE(gate.runtimeJobsOpen());
    TEST_ASSERT_FALSE(gate.tryAcquireUiLease());
    TEST_ASSERT_EQUAL_UINT(1U, gate.uiLeaseCount());
    gate.releaseUiLease();
    TEST_ASSERT_EQUAL_UINT(0U, gate.uiLeaseCount());
    gate.reopen(false);
    TEST_ASSERT_TRUE(gate.ordinaryMutationOpen());
    TEST_ASSERT_TRUE(gate.runtimeJobsOpen());
    TEST_ASSERT_TRUE(gate.normalEventsOpen());
    TEST_ASSERT_FALSE(gate.uiFilesystemOpen());
}

void test_ota_ui_filesystem_gate_prevents_access_until_safe_remount(void) {
    OtaAdmissionGateCore::Gate gate;
    size_t existsCalls = 0U;
    size_t openCalls = 0U;
    const auto serve = [&]() {
        if (!gate.tryAcquireUiLease()) return false;
        ++existsCalls;
        ++openCalls;
        gate.releaseUiLease();
        return true;
    };

    TEST_ASSERT_TRUE(serve());
    gate.close();
    TEST_ASSERT_FALSE(serve());
    TEST_ASSERT_EQUAL_UINT(1U, existsCalls);
    TEST_ASSERT_EQUAL_UINT(1U, openCalls);
    gate.reopen(false);
    TEST_ASSERT_FALSE(serve());
    TEST_ASSERT_EQUAL_UINT(1U, existsCalls);
    TEST_ASSERT_EQUAL_UINT(1U, openCalls);
    gate.close();
    gate.reopen(true);
    TEST_ASSERT_TRUE(serve());
    TEST_ASSERT_EQUAL_UINT(2U, existsCalls);
    TEST_ASSERT_EQUAL_UINT(2U, openCalls);
}

void test_rtu_frame_crc_matches_controller_replies(void) {
    // Exception replies captured from an AHC 9000 answering FC3, FC1 and FC4.
    const uint8_t fc3[] = {0x01, 0x83, 0x01};
    const uint8_t fc1[] = {0x01, 0x81, 0x01};
    const uint8_t fc4[] = {0x01, 0x84, 0x01};
    TEST_ASSERT_EQUAL_HEX16(0xF080U, RtuFrame::crc16(fc3, sizeof fc3));
    TEST_ASSERT_EQUAL_HEX16(0x9081U, RtuFrame::crc16(fc1, sizeof fc1));
    TEST_ASSERT_EQUAL_HEX16(0xC082U, RtuFrame::crc16(fc4, sizeof fc4));

    // The CRC goes on the wire low byte first, and a whole frame checks to
    // zero.
    uint8_t frame[5] = {0x01, 0x83, 0x01};
    TEST_ASSERT_EQUAL_UINT(5U, RtuFrame::appendCrc(frame, 3U));
    TEST_ASSERT_EQUAL_HEX8(0x80U, frame[3]);
    TEST_ASSERT_EQUAL_HEX8(0xF0U, frame[4]);
    TEST_ASSERT_TRUE(RtuFrame::crcValid(frame, sizeof frame));
    frame[2] ^= 0x01U;
    TEST_ASSERT_FALSE(RtuFrame::crcValid(frame, sizeof frame));
}

void test_wavin_frame_builds_read_and_write_requests(void) {
    uint8_t frame[WavinFrame::kMaxWriteRequestSize] = {};
    // Read element 0's air temperature: category 1, index 4, page 0.
    TEST_ASSERT_EQUAL_UINT(8U, WavinFrame::buildRead(1U, 1U, 4U, 0U, 1U, frame, sizeof frame));
    const uint8_t expectedRead[] = {0x01, 0x43, 0x01, 0x04, 0x00, 0x01, 0xC5, 0xF8};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedRead, frame, sizeof expectedRead);

    // Write 21.0 C (210) to channel 3's manual setpoint: category 2, index 0, page 3.
    const uint16_t value = 210U;
    TEST_ASSERT_EQUAL_UINT(10U, WavinFrame::buildWrite(1U, 2U, 0U, 3U, &value, 1U, frame, sizeof frame));
    const uint8_t expectedWrite[] = {0x01, 0x44, 0x02, 0x00, 0x03, 0x01, 0x00, 0xD2, 0x55, 0xF8};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedWrite, frame, sizeof expectedWrite);

    // Counts the frame cannot carry, and buffers that are too small.
    TEST_ASSERT_EQUAL_UINT(0U, WavinFrame::buildRead(1U, 1U, 4U, 0U, 0U, frame, sizeof frame));
    TEST_ASSERT_EQUAL_UINT(0U, WavinFrame::buildRead(1U, 1U, 4U, 0U, WavinFrame::kMaxRegisters + 1U,
                                                     frame, sizeof frame));
    TEST_ASSERT_EQUAL_UINT(0U, WavinFrame::buildRead(1U, 1U, 4U, 0U, 1U, frame, 7U));
    const uint16_t tooMany[WavinFrame::kMaxRegisters + 1U] = {};
    TEST_ASSERT_EQUAL_UINT(0U, WavinFrame::buildWrite(1U, 2U, 0U, 3U, tooMany, WavinFrame::kMaxRegisters + 1U,
                                                      frame, sizeof frame));
    TEST_ASSERT_EQUAL_UINT(8U + 2U * WavinFrame::kMaxRegisters,
                           WavinFrame::buildWrite(1U, 6U, 0U, 3U, tooMany, WavinFrame::kMaxRegisters,
                                                  frame, sizeof frame));
    TEST_ASSERT_EQUAL_UINT(0U, WavinFrame::buildWrite(1U, 2U, 0U, 3U, &value, 1U, frame, 9U));

    // The shapes the scanner is given: a reply carries at most the requested
    // registers (any count up to the maximum for a write), another master's
    // read is 8 bytes and its write is sized by its register count.
    TEST_ASSERT_EQUAL_UINT(9U, WavinFrame::replyShape(2U).maxLength);
    TEST_ASSERT_EQUAL_UINT(WavinFrame::kMaxResponseSize, WavinFrame::replyShape(0U).maxLength);
    TEST_ASSERT_EQUAL_UINT(WavinFrame::kMaxResponseSize, WavinFrame::replyShape(200U).maxLength);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(RtuFrame::FrameShape::Kind::Fixed),
                          static_cast<int>(WavinFrame::peerRequestShape(0x43U).kind));
    TEST_ASSERT_EQUAL_UINT(8U, WavinFrame::peerRequestShape(0x43U).fixedLength);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(RtuFrame::FrameShape::Kind::Counted),
                          static_cast<int>(WavinFrame::peerRequestShape(0x44U).kind));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(RtuFrame::FrameShape::Kind::None),
                          static_cast<int>(WavinFrame::peerRequestShape(0x03U).kind));
}

void test_wavin_frame_writes_whole_clock_page(void) {
    using RtuFrame::ScanStatus;
    const RtuFrame::FrameShape peerWrite = WavinFrame::peerRequestShape(0x44U);
    const RtuFrame::FrameShape anyWrite = WavinFrame::replyShape(WavinFrame::kMaxRegisters);
    // The clock page (category 5) is only accepted whole: 2026-10-17,
    // Saturday (5, Monday is 0), 14:05:30 in one 7-register write.
    const uint16_t clock[7] = {2026U, 10U, 17U, 5U, 14U, 5U, 30U};
    uint8_t frame[WavinFrame::kMaxWriteRequestSize] = {};
    TEST_ASSERT_EQUAL_UINT(22U, WavinFrame::buildWrite(1U, 5U, 0U, 0U, clock, 7U, frame, sizeof frame));
    const uint8_t expected[] = {0x01, 0x44, 0x05, 0x00, 0x00, 0x07, 0x07, 0xEA, 0x00, 0x0A, 0x00,
                                0x11, 0x00, 0x05, 0x00, 0x0E, 0x00, 0x05, 0x00, 0x1E, 0x87, 0x16};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof expected);

    // The controller echoes the written data.
    const uint8_t reply[] = {0x01, 0x44, 0x0E, 0x07, 0xEA, 0x00, 0x0A, 0x00, 0x11, 0x00,
                             0x05, 0x00, 0x0E, 0x00, 0x05, 0x00, 0x1E, 0xB1, 0x3B};
    uint16_t words[WavinFrame::kMaxRegisters] = {};
    const RtuFrame::ScanResult scan = RtuFrame::scan(reply, sizeof reply, 1U, 0x44U, anyWrite, peerWrite);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Response), static_cast<int>(scan.status));
    TEST_ASSERT_EQUAL_UINT(sizeof reply, scan.frameLength);
    TEST_ASSERT_EQUAL_UINT8(7U, WavinFrame::decodeReply(reply, scan.frameLength, words,
                                                        WavinFrame::kMaxRegisters));
    TEST_ASSERT_EQUAL_HEX16_ARRAY(clock, words, 7U);

    // Behind the same write sent by the display, the reply is still found.
    // Category 5 is odd, so the display's frame never looks like a reply and
    // is dropped byte by byte without consulting the peer-request shape.
    uint8_t stream[sizeof expected + sizeof reply];
    std::memcpy(stream, expected, sizeof expected);
    std::memcpy(stream + sizeof expected, reply, sizeof reply);
    std::memset(words, 0, sizeof words);
    RtuFrame::LocateResult located =
        RtuFrame::locate(stream, sizeof stream, 1U, 0x44U, anyWrite, peerWrite);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Response), static_cast<int>(located.status));
    TEST_ASSERT_EQUAL_UINT(sizeof expected, located.dropCount);
    TEST_ASSERT_EQUAL_UINT(sizeof expected, located.frameOffset);
    TEST_ASSERT_EQUAL_UINT(sizeof reply, located.frameLength);
    TEST_ASSERT_FALSE(located.crcMismatch);
    TEST_ASSERT_EQUAL_UINT8(7U, WavinFrame::decodeReply(stream + located.frameOffset, located.frameLength,
                                                        words, WavinFrame::kMaxRegisters));
    TEST_ASSERT_EQUAL_HEX16_ARRAY(clock, words, 7U);

    // A schedule page (category 6) written by the display does read as a
    // reply at first (an even byte count at offset 2), and only its own
    // CRC tells it apart. With write frames of up to kMaxRegisters the
    // scanner recognises the whole 7-word request and drops it at once; at
    // the former four-word limit it was a CRC mismatch and the reply behind
    // it a look-alike.
    const uint16_t schedule[7] = {6U, 0U, 21U, 0U, 7U, 30U, 1U};
    uint8_t peerFrame[WavinFrame::kMaxWriteRequestSize] = {};
    const size_t peerLength = WavinFrame::buildWrite(1U, 6U, 0U, 0U, schedule, 7U, peerFrame,
                                                     sizeof peerFrame);
    TEST_ASSERT_EQUAL_UINT(22U, peerLength);
    const RtuFrame::ScanResult peer = RtuFrame::scan(peerFrame, peerLength, 1U, 0x44U, anyWrite, peerWrite);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Skip), static_cast<int>(peer.status));
    TEST_ASSERT_EQUAL_UINT(peerLength, peer.frameLength);
    TEST_ASSERT_FALSE(peer.crcMismatch);
    uint8_t mixed[sizeof peerFrame + sizeof reply];
    std::memcpy(mixed, peerFrame, peerLength);
    std::memcpy(mixed + peerLength, reply, sizeof reply);
    located = RtuFrame::locate(mixed, peerLength + sizeof reply, 1U, 0x44U, anyWrite, peerWrite);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Response), static_cast<int>(located.status));
    TEST_ASSERT_EQUAL_UINT(peerLength, located.dropCount);
    TEST_ASSERT_EQUAL_UINT(peerLength, located.frameOffset);
    TEST_ASSERT_FALSE(located.crcMismatch);
}

void test_rtu_frame_scans_replies_and_exceptions(void) {
    using RtuFrame::ScanStatus;
    const RtuFrame::FrameShape peerRead = WavinFrame::peerRequestShape(0x43U);
    const RtuFrame::FrameShape peerWrite = WavinFrame::peerRequestShape(0x44U);
    const RtuFrame::FrameShape anyWrite = WavinFrame::replyShape(WavinFrame::kMaxRegisters);
    uint16_t words[4] = {};

    const uint8_t reply[] = {0x01, 0x43, 0x04, 0x00, 0xD2, 0x01, 0x2C, 0x54, 0x87};
    for (size_t partial = 1U; partial < sizeof reply; ++partial) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Incomplete),
                              static_cast<int>(RtuFrame::scan(reply, partial, 1U, 0x43U,
                                                              WavinFrame::replyShape(2U), peerRead).status));
    }
    RtuFrame::ScanResult scan =
        RtuFrame::scan(reply, sizeof reply, 1U, 0x43U, WavinFrame::replyShape(2U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Response), static_cast<int>(scan.status));
    TEST_ASSERT_EQUAL_UINT(sizeof reply, scan.frameLength);
    TEST_ASSERT_EQUAL_UINT8(2U, WavinFrame::decodeReply(reply, scan.frameLength, words, 4U));
    TEST_ASSERT_EQUAL_UINT16(210U, words[0]);
    TEST_ASSERT_EQUAL_UINT16(300U, words[1]);
    // A reply with more registers than fit the caller's words still reports
    // how many it carried; one too short to be a reply reports none.
    TEST_ASSERT_EQUAL_UINT8(2U, WavinFrame::decodeReply(reply, scan.frameLength, words, 1U));
    TEST_ASSERT_EQUAL_UINT8(0U, WavinFrame::decodeReply(reply, 4U, words, 4U));

    // A reply with more words than requested is not ours (a second master's
    // traffic); one with fewer is what the controller answers on some pages.
    scan = RtuFrame::scan(reply, sizeof reply, 1U, 0x43U, WavinFrame::replyShape(1U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Skip), static_cast<int>(scan.status));
    TEST_ASSERT_FALSE(scan.crcMismatch);
    scan = RtuFrame::scan(reply, sizeof reply, 1U, 0x43U, WavinFrame::replyShape(4U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Response), static_cast<int>(scan.status));
    TEST_ASSERT_EQUAL_UINT(sizeof reply, scan.frameLength);
    // An odd byte count is not a register reply either.
    uint8_t oddCount[] = {0x01, 0x43, 0x03, 0x00, 0xD2, 0x01, 0x00, 0x00};
    RtuFrame::appendCrc(oddCount, 6U);
    scan = RtuFrame::scan(oddCount, sizeof oddCount, 1U, 0x43U, WavinFrame::replyShape(4U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Skip), static_cast<int>(scan.status));
    TEST_ASSERT_FALSE(scan.crcMismatch);
    // Writes accept any word count.
    uint8_t writeReply[] = {0x01, 0x44, 0x02, 0x00, 0xD2, 0x00, 0x00};
    RtuFrame::appendCrc(writeReply, 5U);
    scan = RtuFrame::scan(writeReply, sizeof writeReply, 1U, 0x44U, anyWrite, peerWrite);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Response), static_cast<int>(scan.status));
    TEST_ASSERT_EQUAL_UINT(sizeof writeReply, scan.frameLength);

    // Corrupted CRC.
    uint8_t corrupted[sizeof reply];
    std::memcpy(corrupted, reply, sizeof reply);
    corrupted[4] ^= 0x01U;
    scan = RtuFrame::scan(corrupted, sizeof corrupted, 1U, 0x43U, WavinFrame::replyShape(2U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Skip), static_cast<int>(scan.status));
    TEST_ASSERT_TRUE(scan.crcMismatch);

    // Other slaves and other functions are skipped byte by byte.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Skip),
                          static_cast<int>(RtuFrame::scan(reply, sizeof reply, 2U, 0x43U,
                                                          WavinFrame::replyShape(2U), peerRead).status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Skip),
                          static_cast<int>(RtuFrame::scan(reply, sizeof reply, 1U, 0x44U, anyWrite,
                                                          peerWrite).status));

    // Exception reply from the controller.
    uint8_t exceptionFrame[] = {0x01, 0xC3, 0x02, 0x00, 0x00};
    RtuFrame::appendCrc(exceptionFrame, 3U);
    scan = RtuFrame::scan(exceptionFrame, sizeof exceptionFrame, 1U, 0x43U, WavinFrame::replyShape(2U),
                          peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Exception), static_cast<int>(scan.status));
    TEST_ASSERT_EQUAL_UINT8(0x02U, scan.exceptionCode);
    TEST_ASSERT_EQUAL_UINT(RtuFrame::kExceptionSize, scan.frameLength);
    exceptionFrame[2] ^= 0x01U;
    scan = RtuFrame::scan(exceptionFrame, sizeof exceptionFrame, 1U, 0x43U, WavinFrame::replyShape(2U),
                          peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Skip), static_cast<int>(scan.status));
    TEST_ASSERT_TRUE(scan.crcMismatch);

    // Our reply after another master's request to the same slave: the request
    // (byte count taken from its category) fails its CRC as a reply, is
    // recognised by its own CRC and skipped whole, without counting as a
    // corrupted reply.
    uint8_t stream[8U + sizeof reply];
    WavinFrame::buildRead(1U, 0x02U, 0x00U, 0x05U, 0x0BU, stream, 8U);
    std::memcpy(stream + 8U, reply, sizeof reply);
    scan = RtuFrame::scan(stream, sizeof stream, 1U, 0x43U, WavinFrame::replyShape(2U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Skip), static_cast<int>(scan.status));
    TEST_ASSERT_EQUAL_UINT(8U, scan.frameLength);
    TEST_ASSERT_FALSE(scan.crcMismatch);
    scan = RtuFrame::scan(stream + 8U, sizeof stream - 8U, 1U, 0x43U, WavinFrame::replyShape(2U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Response), static_cast<int>(scan.status));
    // Without a shape for the other master's requests, the same bytes can
    // only be a corrupted reply.
    scan = RtuFrame::scan(stream, sizeof stream, 1U, 0x43U, WavinFrame::replyShape(2U),
                          RtuFrame::FrameShape::none());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Skip), static_cast<int>(scan.status));
    TEST_ASSERT_EQUAL_UINT(1U, scan.frameLength);
    TEST_ASSERT_TRUE(scan.crcMismatch);

    // Until the whole request has arrived, its failed reply CRC is not yet a
    // corrupted reply; once no more bytes can come, it is.
    scan = RtuFrame::scan(stream, 7U, 1U, 0x43U, WavinFrame::replyShape(2U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Incomplete), static_cast<int>(scan.status));
    scan = RtuFrame::scan(stream, 7U, 1U, 0x43U, WavinFrame::replyShape(2U), peerRead, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Skip), static_cast<int>(scan.status));
    TEST_ASSERT_TRUE(scan.crcMismatch);
    // The same for a write request from the display (10 bytes for one word).
    uint8_t writeRequest[WavinFrame::kMaxWriteRequestSize];
    const uint16_t setpoint = 215U;
    TEST_ASSERT_EQUAL_UINT(10U, WavinFrame::buildWrite(1U, 0x02U, 0x00U, 0x05U, &setpoint, 1U,
                                                       writeRequest, sizeof writeRequest));
    scan = RtuFrame::scan(writeRequest, 9U, 1U, 0x44U, anyWrite, peerWrite);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Incomplete), static_cast<int>(scan.status));
    scan = RtuFrame::scan(writeRequest, 10U, 1U, 0x44U, anyWrite, peerWrite);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Skip), static_cast<int>(scan.status));
    TEST_ASSERT_EQUAL_UINT(10U, scan.frameLength);
    TEST_ASSERT_FALSE(scan.crcMismatch);

    // A fixed-length reply shape (standard Modbus write echoes) is complete
    // at exactly its length.
    uint8_t echo[] = {0x01, 0x06, 0x00, 0x10, 0x00, 0x2A, 0x00, 0x00};
    RtuFrame::appendCrc(echo, 6U);
    scan = RtuFrame::scan(echo, 7U, 1U, 0x06U, RtuFrame::FrameShape::fixed(8U), RtuFrame::FrameShape::none());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Incomplete), static_cast<int>(scan.status));
    scan = RtuFrame::scan(echo, 8U, 1U, 0x06U, RtuFrame::FrameShape::fixed(8U), RtuFrame::FrameShape::none());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Response), static_cast<int>(scan.status));
    TEST_ASSERT_EQUAL_UINT(8U, scan.frameLength);
}

void test_rtu_frame_locates_reply_among_other_traffic(void) {
    using RtuFrame::ScanStatus;
    const RtuFrame::FrameShape peerRead = WavinFrame::peerRequestShape(0x43U);
    const RtuFrame::FrameShape peerWrite = WavinFrame::peerRequestShape(0x44U);
    const RtuFrame::FrameShape anyWrite = WavinFrame::replyShape(WavinFrame::kMaxRegisters);
    uint16_t words[4] = {};
    const uint8_t reply[] = {0x01, 0x43, 0x04, 0x00, 0xD2, 0x01, 0x2C, 0x54, 0x87};

    // The reply alone, byte by byte.
    for (size_t partial = 1U; partial < sizeof reply; ++partial) {
        const RtuFrame::LocateResult located =
            RtuFrame::locate(reply, partial, 1U, 0x43U, WavinFrame::replyShape(2U), peerRead);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Incomplete), static_cast<int>(located.status));
        TEST_ASSERT_EQUAL_UINT(0U, located.dropCount);
        TEST_ASSERT_FALSE(located.crcMismatch);
    }
    RtuFrame::LocateResult located =
        RtuFrame::locate(reply, sizeof reply, 1U, 0x43U, WavinFrame::replyShape(2U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Response), static_cast<int>(located.status));
    TEST_ASSERT_EQUAL_UINT(0U, located.frameOffset);
    TEST_ASSERT_EQUAL_UINT(sizeof reply, located.frameLength);
    TEST_ASSERT_EQUAL_UINT8(2U, WavinFrame::decodeReply(reply + located.frameOffset, located.frameLength,
                                                        words, 4U));
    TEST_ASSERT_EQUAL_UINT16(210U, words[0]);

    // Stale bytes at the head that claim the largest frame (22 registers,
    // 44 data bytes) must not hide the write reply behind them: the reply is
    // found, not timed out.
    uint8_t writeReply[] = {0x01, 0x44, 0x02, 0x00, 0xD2, 0x00, 0x00};
    RtuFrame::appendCrc(writeReply, 5U);
    uint8_t stale[3U + sizeof writeReply] = {0x01, 0x44, 2U * WavinFrame::kMaxRegisters};
    std::memcpy(stale + 3U, writeReply, sizeof writeReply);
    located = RtuFrame::locate(stale, 3U, 1U, 0x44U, anyWrite, peerWrite);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Incomplete), static_cast<int>(located.status));
    TEST_ASSERT_EQUAL_UINT(0U, located.dropCount);
    located = RtuFrame::locate(stale, sizeof stale, 1U, 0x44U, anyWrite, peerWrite);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Response), static_cast<int>(located.status));
    TEST_ASSERT_EQUAL_UINT(3U, located.frameOffset);
    TEST_ASSERT_EQUAL_UINT(sizeof writeReply, located.frameLength);
    TEST_ASSERT_FALSE(located.crcMismatch);

    // The display's request to the same slave, then nothing: a timeout, not
    // a CRC error, and the request's bytes are dropped once recognised.
    uint8_t request[8];
    WavinFrame::buildRead(1U, 0x02U, 0x00U, 0x05U, 0x0BU, request, sizeof request);
    located = RtuFrame::locate(request, 7U, 1U, 0x43U, WavinFrame::replyShape(1U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Incomplete), static_cast<int>(located.status));
    TEST_ASSERT_FALSE(located.crcMismatch);
    located = RtuFrame::locate(request, sizeof request, 1U, 0x43U, WavinFrame::replyShape(1U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Incomplete), static_cast<int>(located.status));
    TEST_ASSERT_EQUAL_UINT(sizeof request, located.dropCount);
    TEST_ASSERT_FALSE(located.crcMismatch);
    located = RtuFrame::locate(request, sizeof request, 1U, 0x43U, WavinFrame::replyShape(1U), peerRead,
                               true);
    TEST_ASSERT_FALSE(located.crcMismatch);

    // A reply of ours with a flipped bit is a CRC error, also when it arrived
    // behind other traffic, and its bytes are dropped.
    uint8_t corrupted[sizeof reply];
    std::memcpy(corrupted, reply, sizeof reply);
    corrupted[4] ^= 0x01U;
    located = RtuFrame::locate(corrupted, sizeof corrupted, 1U, 0x43U, WavinFrame::replyShape(2U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Incomplete), static_cast<int>(located.status));
    TEST_ASSERT_TRUE(located.crcMismatch);
    TEST_ASSERT_EQUAL_UINT(sizeof corrupted, located.dropCount);
    uint8_t mixed[sizeof request + sizeof corrupted];
    std::memcpy(mixed, request, sizeof request);
    std::memcpy(mixed + sizeof request, corrupted, sizeof corrupted);
    located = RtuFrame::locate(mixed, sizeof mixed, 1U, 0x43U, WavinFrame::replyShape(2U), peerRead, true);
    TEST_ASSERT_TRUE(located.crcMismatch);

    // A short corrupted reply (one register, 7 bytes) could be the start of a
    // request until the line goes quiet.
    uint8_t shortReply[] = {0x01, 0x43, 0x02, 0x00, 0xD2, 0x00, 0x00};
    RtuFrame::appendCrc(shortReply, 5U);
    shortReply[6] ^= 0x01U;
    located = RtuFrame::locate(shortReply, sizeof shortReply, 1U, 0x43U, WavinFrame::replyShape(1U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Incomplete), static_cast<int>(located.status));
    TEST_ASSERT_FALSE(located.crcMismatch);
    TEST_ASSERT_EQUAL_UINT(0U, located.dropCount);
    located = RtuFrame::locate(shortReply, sizeof shortReply, 1U, 0x43U, WavinFrame::replyShape(1U), peerRead,
                               true);
    TEST_ASSERT_TRUE(located.crcMismatch);

    // An exception reply behind another slave's frame is found too.
    uint8_t exceptionFrame[] = {0x01, 0xC3, 0x02, 0x00, 0x00};
    RtuFrame::appendCrc(exceptionFrame, 3U);
    uint8_t behind[2U + sizeof exceptionFrame] = {0x05, 0x43};
    std::memcpy(behind + 2U, exceptionFrame, sizeof exceptionFrame);
    located = RtuFrame::locate(behind, sizeof behind, 1U, 0x43U, WavinFrame::replyShape(2U), peerRead);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScanStatus::Exception), static_cast<int>(located.status));
    TEST_ASSERT_EQUAL_UINT8(0x02U, located.exceptionCode);
    TEST_ASSERT_EQUAL_UINT(2U, located.frameOffset);
    TEST_ASSERT_EQUAL_UINT(2U, located.dropCount);
}

void test_rtu_timing_scales_with_baud_and_fits_budget(void) {
    // Wire time at 8N1 (10 bits per character), rounded up to whole ms.
    TEST_ASSERT_EQUAL_UINT32(13U, RtuTiming::wireTimeMs(49U, 38400U, 10U));
    TEST_ASSERT_EQUAL_UINT32(52U, RtuTiming::wireTimeMs(49U, 9600U, 10U));
    TEST_ASSERT_EQUAL_UINT32(409U, RtuTiming::wireTimeMs(49U, 1200U, 10U));
    TEST_ASSERT_EQUAL_UINT32(3U, RtuTiming::wireTimeMs(WavinFrame::kReadRequestSize, 38400U, 10U));
    // 8E2 has 12 bits per character; an unset baud rate costs nothing rather
    // than dividing by zero.
    TEST_ASSERT_EQUAL_UINT32(490U, RtuTiming::wireTimeMs(49U, 1200U, 12U));
    TEST_ASSERT_EQUAL_UINT32(0U, RtuTiming::wireTimeMs(49U, 0U, 10U));

    // Reply sizes: header (3) + data + CRC (2).
    TEST_ASSERT_EQUAL_UINT(7U, WavinFrame::responseSize(1U));
    TEST_ASSERT_EQUAL_UINT(49U, WavinFrame::responseSize(WavinFrame::kMaxRegisters));
    TEST_ASSERT_EQUAL_UINT(WavinFrame::kMaxResponseSize,
                           WavinFrame::responseSize(WavinFrame::kMaxRegisters));

    // One attempt waits for the turnaround plus the largest reply on the wire.
    TEST_ASSERT_EQUAL_UINT32(502U, RtuTiming::attemptTimeoutMs(500U, WavinFrame::responseSize(1U), 38400U, 10U));
    TEST_ASSERT_EQUAL_UINT32(513U, RtuTiming::attemptTimeoutMs(500U, WavinFrame::responseSize(22U), 38400U, 10U));
    TEST_ASSERT_EQUAL_UINT32(508U, RtuTiming::attemptTimeoutMs(500U, WavinFrame::responseSize(1U), 9600U, 10U));
    TEST_ASSERT_EQUAL_UINT32(552U, RtuTiming::attemptTimeoutMs(500U, WavinFrame::responseSize(22U), 9600U, 10U));
    TEST_ASSERT_EQUAL_UINT32(559U, RtuTiming::attemptTimeoutMs(500U, WavinFrame::responseSize(1U), 1200U, 10U));
    TEST_ASSERT_EQUAL_UINT32(909U, RtuTiming::attemptTimeoutMs(500U, WavinFrame::responseSize(22U), 1200U, 10U));

    // A retry fits when the time spent, the pause and a full attempt stay
    // within the budget; the comparison does not overflow.
    TEST_ASSERT_TRUE(RtuTiming::attemptFits(1000U, 250U, 750U, 2000U));
    TEST_ASSERT_FALSE(RtuTiming::attemptFits(1000U, 250U, 751U, 2000U));
    TEST_ASSERT_FALSE(RtuTiming::attemptFits(0xFFFFFFFFU, 1U, 1U, 2000U));

    // Attempts a read gets when every attempt times out, with the configured
    // turnaround, budget and attempt limit: the first always, each retry
    // only if its pause and a full attempt (request and reply on the wire
    // plus the turnaround) still fit. The pause is random, so the count is
    // given for the shortest and the longest pause.
    const auto attempts = [](const uint8_t words, const uint32_t baud, const uint32_t pauseMs) {
        const uint32_t attemptMs =
            RtuTiming::wireTimeMs(WavinFrame::kReadRequestSize, baud, 10U)
            + RtuTiming::attemptTimeoutMs(WAVIN_TURNAROUND_MS, WavinFrame::responseSize(words), baud, 10U);
        uint32_t elapsed = attemptMs;
        uint8_t count = 1U;
        while (count < WAVIN_READ_ATTEMPTS
               && RtuTiming::attemptFits(elapsed, pauseMs, attemptMs, WAVIN_TRANSACTION_BUDGET_MS)) {
            elapsed += pauseMs + attemptMs;
            ++count;
        }
        return count;
    };
    constexpr uint32_t shortest = WAVIN_RETRY_BACKOFF_MIN_MS;
    constexpr uint32_t longest = WAVIN_RETRY_BACKOFF_MAX_MS;
    // 38400 8N1, what the AHC 9000 uses: all three attempts unless both
    // pauses draw near their maximum (3 x 500 + 2 x 250 is the whole budget).
    TEST_ASSERT_EQUAL_UINT8(3U, attempts(1U, 38400U, shortest));
    TEST_ASSERT_EQUAL_UINT8(3U, attempts(22U, 38400U, shortest));
    TEST_ASSERT_EQUAL_UINT8(2U, attempts(1U, 38400U, longest));
    TEST_ASSERT_EQUAL_UINT8(2U, attempts(22U, 38400U, longest));
    // 9600 8N1, the firmware default.
    TEST_ASSERT_EQUAL_UINT8(3U, attempts(1U, 9600U, shortest));
    TEST_ASSERT_EQUAL_UINT8(3U, attempts(22U, 9600U, shortest));
    TEST_ASSERT_EQUAL_UINT8(2U, attempts(1U, 9600U, longest));
    TEST_ASSERT_EQUAL_UINT8(2U, attempts(22U, 9600U, longest));
    // 1200 8N1, the lowest rate the bus form accepts: a full read gets one
    // attempt, a single register two.
    TEST_ASSERT_EQUAL_UINT8(2U, attempts(1U, 1200U, shortest));
    TEST_ASSERT_EQUAL_UINT8(2U, attempts(1U, 1200U, longest));
    TEST_ASSERT_EQUAL_UINT8(1U, attempts(22U, 1200U, shortest));
    TEST_ASSERT_EQUAL_UINT8(1U, attempts(22U, 1200U, longest));

    // The first attempt always fits, even the largest reply at the lowest
    // baud rate with the largest characters (8E2), so no datapoint becomes
    // unreadable; WavinClient.cpp asserts the same at compile time.
    TEST_ASSERT_TRUE(RtuTiming::attemptFits(
        0U, 0U,
        RtuTiming::wireTimeMs(WavinFrame::kMaxWriteRequestSize, MODBUS_MIN_BAUD_RATE,
                              RtuTiming::kMaxBitsPerCharacter)
            + RtuTiming::attemptTimeoutMs(WAVIN_TURNAROUND_MS, WavinFrame::kMaxResponseSize,
                                          MODBUS_MIN_BAUD_RATE, RtuTiming::kMaxBitsPerCharacter),
        WAVIN_TRANSACTION_BUDGET_MS));
}

// A scripted RS-485 line for the transport. Frames sent are recorded; the
// bytes scripted for the n-th send arrive the given delay after it has
// left the wire. Time only passes when the transport idles, so a test is
// deterministic and runs instantly.
class FakeRtuLink : public RtuLink {
public:
    struct Arrival {
        uint32_t delayMs;
        std::vector<uint8_t> bytes;
    };

    uint32_t baud{38400U};
    uint8_t bits{10U};
    uint32_t randomValue{0U};
    uint32_t now{1000U};
    std::vector<std::vector<uint8_t>> sent;
    // replies[n] is what arrives after the (n + 1)-th send.
    std::vector<std::vector<Arrival>> replies;

    uint32_t baudRate() const override { return baud; }
    uint8_t bitsPerCharacter() const override { return bits; }

    void send(const uint8_t *data, const size_t length) override {
        sent.emplace_back(data, data + length);
        now += RtuTiming::wireTimeMs(length, baud, bits);
        const size_t index = sent.size() - 1U;
        if (index < replies.size()) {
            for (const Arrival &arrival : replies[index]) schedule(now + arrival.delayMs, arrival.bytes);
        }
    }

    void discardInput() override {
        while (read() >= 0) {
        }
    }

    int read() override {
        if (_pending.empty() || _pending.front().first > now) return -1;
        const int value = _pending.front().second;
        _pending.erase(_pending.begin());
        return value;
    }

    uint32_t nowMs() const override { return now; }
    void idleMs(const uint32_t ms) override { now += ms; }
    uint32_t randomWord() override { return randomValue; }

    // Bytes scripted but not yet arrived.
    size_t undelivered() const {
        size_t count = 0U;
        for (const auto &entry : _pending) count += entry.first > now ? 1U : 0U;
        return count;
    }

private:
    void schedule(const uint32_t atMs, const std::vector<uint8_t> &bytes) {
        for (const uint8_t value : bytes) _pending.emplace_back(atMs, value);
        std::stable_sort(_pending.begin(), _pending.end(),
                         [](const std::pair<uint32_t, uint8_t> &a, const std::pair<uint32_t, uint8_t> &b) {
                             return a.first < b.first;
                         });
    }

    std::vector<std::pair<uint32_t, uint8_t>> _pending;
};

namespace {

constexpr RtuPolicy kTestPolicy = {500U, 2000U, 100U, 250U, 50U, 150U};

RtuRequest wavinReadRequest(const uint8_t *frame, const size_t length, const uint8_t count,
                            const uint8_t attempts) {
    RtuRequest request{};
    request.frame = frame;
    request.length = length;
    request.slave = 1U;
    request.function = WavinFrame::kReadFunction;
    request.reply = WavinFrame::replyShape(count);
    request.peerRequest = WavinFrame::peerRequestShape(WavinFrame::kReadFunction);
    request.maxReplyLength = request.reply.maxLength;
    request.attempts = attempts;
    return request;
}

std::vector<uint8_t> framed(std::vector<uint8_t> bytes) {
    bytes.resize(bytes.size() + RtuFrame::kCrcSize);
    RtuFrame::appendCrc(bytes.data(), bytes.size() - RtuFrame::kCrcSize);
    return bytes;
}

}  // namespace

void test_rtu_transport_returns_the_reply_found_among_other_traffic(void) {
    FakeRtuLink link;
    uint8_t request[WavinFrame::kReadRequestSize];
    TEST_ASSERT_EQUAL_UINT(8U, WavinFrame::buildRead(1U, 1U, 4U, 0U, 2U, request, sizeof request));
    // On the line 5 ms after the request: the display's request to the same
    // slave, a reply to another slave, then our reply.
    std::vector<uint8_t> traffic(8U);
    WavinFrame::buildRead(1U, 0x02U, 0x00U, 0x05U, 0x0BU, traffic.data(), traffic.size());
    const std::vector<uint8_t> otherSlave = framed({0x02, 0x43, 0x02, 0x00, 0x01});
    const std::vector<uint8_t> reply = {0x01, 0x43, 0x04, 0x00, 0xD2, 0x01, 0x2C, 0x54, 0x87};
    traffic.insert(traffic.end(), otherSlave.begin(), otherSlave.end());
    traffic.insert(traffic.end(), reply.begin(), reply.end());
    link.replies = {{{5U, traffic}}};

    RtuTransport transport(link, kTestPolicy);
    uint8_t frame[WavinFrame::kMaxResponseSize] = {};
    size_t length = 0U;
    uint32_t startedAt = link.now;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess,
                           transport.exchange(wavinReadRequest(request, sizeof request, 2U, 3U), frame,
                                              sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(reply.size(), length);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(reply.data(), frame, reply.size());
    TEST_ASSERT_EQUAL_UINT(1U, link.sent.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(request, link.sent[0].data(), sizeof request);
    // Returned as soon as the reply was complete: the request's wire time
    // plus the 5 ms, not the timeout.
    TEST_ASSERT_EQUAL_UINT32(RtuTiming::wireTimeMs(8U, 38400U, 10U) + 5U, link.now - startedAt);
    uint16_t words[2] = {};
    TEST_ASSERT_EQUAL_UINT8(2U, WavinFrame::decodeReply(frame, length, words, 2U));
    TEST_ASSERT_EQUAL_UINT16(210U, words[0]);
    TEST_ASSERT_EQUAL_UINT16(300U, words[1]);

    // A reply arriving in two bursts, with idle polls in between, is complete
    // when its last byte is.
    FakeRtuLink split;
    split.replies = {{{5U, {0x01, 0x43, 0x04, 0x00}}, {9U, {0xD2, 0x01, 0x2C, 0x54, 0x87}}}};
    startedAt = split.now;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess,
                           RtuTransport(split, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 3U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(reply.size(), length);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(reply.data(), frame, reply.size());
    TEST_ASSERT_EQUAL_UINT32(RtuTiming::wireTimeMs(8U, 38400U, 10U) + 9U, split.now - startedAt);

    // The caller's buffer must hold the largest reply of the shape: one
    // that does is enough, one byte short (or none) is a caller error
    // reported before anything is sent, never a cut reply passed as whole.
    FakeRtuLink small;
    small.replies = {{{5U, reply}}};
    uint8_t exact[WavinFrame::responseSize(2U)] = {};
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess,
                           RtuTransport(small, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 3U), exact, sizeof exact, length));
    TEST_ASSERT_EQUAL_UINT(reply.size(), length);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(reply.data(), exact, reply.size());
    uint8_t tiny[WavinFrame::responseSize(2U) - 1U] = {};
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataValue,
                           RtuTransport(small, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 3U), tiny, sizeof tiny, length));
    TEST_ASSERT_EQUAL_UINT(0U, length);
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataValue,
                           RtuTransport(small, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 3U), nullptr, 0U, length));
    TEST_ASSERT_EQUAL_UINT(1U, small.sent.size());
}

void test_rtu_transport_retries_timeouts_and_crc_errors_within_the_budget(void) {
    uint8_t request[WavinFrame::kReadRequestSize];
    WavinFrame::buildRead(1U, 1U, 4U, 0U, 2U, request, sizeof request);
    const std::vector<uint8_t> reply = {0x01, 0x43, 0x04, 0x00, 0xD2, 0x01, 0x2C, 0x54, 0x87};
    const uint32_t requestMs = RtuTiming::wireTimeMs(8U, 38400U, 10U);
    const uint32_t timeoutMs = RtuTiming::attemptTimeoutMs(500U, WavinFrame::responseSize(2U), 38400U, 10U);
    uint8_t frame[WavinFrame::kMaxResponseSize] = {};
    size_t length = 0U;

    // A silent first attempt, then the reply: two sends with the drawn
    // backoff between them. randomValue 0 draws the shortest pause.
    FakeRtuLink link;
    link.replies = {{}, {{5U, reply}}};
    uint32_t startedAt = link.now;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess,
                           RtuTransport(link, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 3U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(2U, link.sent.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(reply.data(), frame, reply.size());
    TEST_ASSERT_EQUAL_UINT32(requestMs + timeoutMs + 100U + requestMs + 5U, link.now - startedAt);

    // A frame that lands during the backoff pause (the display's reply from
    // the controller, which carries no address of its own and would pass
    // for ours) is drained before the retry is sent, so the retry gets its
    // own reply: one register, 42, not the two words of the stale frame.
    FakeRtuLink stale;
    stale.replies = {{{timeoutMs + 30U, framed({0x01, 0x43, 0x04, 0x12, 0x34, 0x56, 0x78})}},
                     {{5U, framed({0x01, 0x43, 0x02, 0x00, 0x2A})}}};
    startedAt = stale.now;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess,
                           RtuTransport(stale, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 3U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(2U, stale.sent.size());
    uint16_t words[2] = {};
    TEST_ASSERT_EQUAL_UINT8(1U, WavinFrame::decodeReply(frame, length, words, 2U));
    TEST_ASSERT_EQUAL_UINT16(42U, words[0]);
    TEST_ASSERT_EQUAL_UINT32(requestMs + timeoutMs + 100U + requestMs + 5U, stale.now - startedAt);

    // A corrupted reply is retried like a timeout.
    std::vector<uint8_t> corrupted = reply;
    corrupted[4] ^= 0x01U;
    FakeRtuLink crc;
    crc.replies = {{{5U, corrupted}}, {{5U, reply}}};
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess,
                           RtuTransport(crc, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 3U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(2U, crc.sent.size());
    // ... and reported when it was the last attempt.
    FakeRtuLink crcOnly;
    crcOnly.replies = {{{5U, corrupted}}};
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kInvalidCrc,
                           RtuTransport(crcOnly, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 1U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(1U, crcOnly.sent.size());
    TEST_ASSERT_EQUAL_UINT(0U, length);
    // A corrupted reply shorter than the display's request cannot be told
    // from the start of such a request while bytes may still come; once the
    // attempt has timed out it is known to be corrupted and reported as such.
    std::vector<uint8_t> shortCorrupted = framed({0x01, 0x43, 0x02, 0x00, 0x2A});
    shortCorrupted[4] ^= 0x01U;
    FakeRtuLink shortCrc;
    shortCrc.replies = {{{5U, shortCorrupted}}};
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kInvalidCrc,
                           RtuTransport(shortCrc, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 1U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(1U, shortCrc.sent.size());
    TEST_ASSERT_EQUAL_UINT(0U, length);

    // Nothing ever answers. With the shortest pauses all three attempts fit
    // the 2000 ms budget; with the longest (randomValue 150 draws 250 ms)
    // the third does not, so the failure is final after two. The clock
    // wraps during the first case, as millis() does after 49 days.
    FakeRtuLink silent;
    silent.now = 0xFFFFFF00U;
    startedAt = silent.now;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kResponseTimedOut,
                           RtuTransport(silent, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 3U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(3U, silent.sent.size());
    TEST_ASSERT_EQUAL_UINT32(3U * (requestMs + timeoutMs) + 2U * 100U, silent.now - startedAt);
    TEST_ASSERT_TRUE(silent.now < startedAt);
    FakeRtuLink slow;
    slow.randomValue = 150U;
    startedAt = slow.now;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kResponseTimedOut,
                           RtuTransport(slow, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 3U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(2U, slow.sent.size());
    TEST_ASSERT_EQUAL_UINT32(2U * (requestMs + timeoutMs) + 250U, slow.now - startedAt);
    // A single attempt is never repeated.
    FakeRtuLink once;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kResponseTimedOut,
                           RtuTransport(once, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 1U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(1U, once.sent.size());

    // At 1200 baud a full 22-register read costs 976 ms per attempt, so no
    // retry fits and the second attempt is never sent.
    FakeRtuLink low;
    low.baud = 1200U;
    uint8_t full[WavinFrame::kReadRequestSize];
    WavinFrame::buildRead(1U, 1U, 0U, 0U, WavinFrame::kMaxRegisters, full, sizeof full);
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kResponseTimedOut,
                           RtuTransport(low, kTestPolicy).exchange(
                               wavinReadRequest(full, sizeof full, WavinFrame::kMaxRegisters, 3U), frame,
                               sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(1U, low.sent.size());
}

void test_rtu_transport_exceptions_are_final_and_the_line_is_settled_after_a_failure(void) {
    uint8_t request[WavinFrame::kReadRequestSize];
    WavinFrame::buildRead(1U, 1U, 4U, 0U, 2U, request, sizeof request);
    const uint32_t requestMs = RtuTiming::wireTimeMs(8U, 38400U, 10U);
    const uint32_t timeoutMs = RtuTiming::attemptTimeoutMs(500U, WavinFrame::responseSize(2U), 38400U, 10U);
    uint8_t frame[WavinFrame::kMaxResponseSize] = {};
    size_t length = 0U;

    // An exception reply is the controller's answer: returned at once, no
    // retry, nothing more read.
    FakeRtuLink link;
    link.replies = {{{5U, framed({0x01, 0xC3, 0x02})}}};
    TEST_ASSERT_EQUAL_HEX8(0x02U,
                           RtuTransport(link, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 3U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT(1U, link.sent.size());
    TEST_ASSERT_EQUAL_UINT(0U, length);

    // The attempt sees the start of another slave's frame shortly before it
    // times out, and the rest of that burst plus a late reply of ours land
    // just after. The settle wait discards them until the line has been
    // quiet for 50 ms, so the next exchange does not take the late reply
    // for its own; a byte arriving after the wait has returned is left for
    // the next exchange's drain.
    const std::vector<uint8_t> lateReply = {0x01, 0x43, 0x04, 0x00, 0xD2, 0x01, 0x2C, 0x54, 0x87};
    FakeRtuLink late;
    late.replies = {{{timeoutMs - 20U, {0x02, 0x43, 0x02}},
                     {timeoutMs + 4U, {0x00, 0x01, 0x00, 0x00}},
                     {timeoutMs + 8U, lateReply},
                     {timeoutMs + 400U, {0xAA}}},
                    {{5U, framed({0x01, 0x43, 0x02, 0x00, 0x2A})}}};
    uint32_t startedAt = late.now;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kResponseTimedOut,
                           RtuTransport(late, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 1U), frame, sizeof frame, length));
    // Settled 50 ms after the last byte of the late reply, within the cap.
    TEST_ASSERT_EQUAL_UINT32(requestMs + timeoutMs + 8U + 50U, late.now - startedAt);
    TEST_ASSERT_EQUAL_UINT(1U, late.undelivered());
    TEST_ASSERT_EQUAL_INT(-1, late.read());
    // The next exchange gets its own reply (one register, 42), not the late one.
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess,
                           RtuTransport(late, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 1U), frame, sizeof frame, length));
    uint16_t words[2] = {};
    TEST_ASSERT_EQUAL_UINT8(1U, WavinFrame::decodeReply(frame, length, words, 2U));
    TEST_ASSERT_EQUAL_UINT16(42U, words[0]);

    // After a silent timeout nothing was received, so the settle wait
    // returns at once.
    FakeRtuLink silent;
    startedAt = silent.now;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kResponseTimedOut,
                           RtuTransport(silent, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 1U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT32(requestMs + timeoutMs, silent.now - startedAt);

    // Another master's frame ends just before the timeout and nothing
    // follows: the quiet window is measured from its last byte, not from
    // the first byte the attempt saw.
    FakeRtuLink tail;
    tail.replies = {{{timeoutMs - 20U, {0x02, 0x43}}, {timeoutMs - 2U, {0x02, 0x00, 0x01}}}};
    startedAt = tail.now;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kResponseTimedOut,
                           RtuTransport(tail, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 1U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT32(requestMs + timeoutMs - 2U + 50U, tail.now - startedAt);

    // A talker that never stops is given up on after the 150 ms cap.
    FakeRtuLink chatter;
    std::vector<FakeRtuLink::Arrival> stream;
    for (uint32_t at = timeoutMs - 10U; at < timeoutMs + 400U; at += 10U) stream.push_back({at, {0x02}});
    chatter.replies = {stream};
    startedAt = chatter.now;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kResponseTimedOut,
                           RtuTransport(chatter, kTestPolicy).exchange(
                               wavinReadRequest(request, sizeof request, 2U, 1U), frame, sizeof frame, length));
    TEST_ASSERT_EQUAL_UINT32(requestMs + timeoutMs + 150U, chatter.now - startedAt);
}

void test_wavin_client_reads_and_writes_through_the_transport(void) {
    FakeRtuLink link;
    const std::vector<uint8_t> readReply = {0x01, 0x43, 0x04, 0x00, 0xD2, 0x01, 0x2C, 0x54, 0x87};
    link.replies = {{{5U, readReply}}, {{5U, framed({0x01, 0x44, 0x02, 0x00, 0xD2})}}, {{5U, readReply}}};
    WavinClient client(link);

    // A read decodes the registers the reply carried.
    uint16_t words[4] = {};
    uint8_t received = 0U;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, client.readRegisters(1U, 1U, 4U, 0U, 2U, words, 4U, received));
    TEST_ASSERT_EQUAL_UINT8(2U, received);
    TEST_ASSERT_EQUAL_UINT16(210U, words[0]);
    TEST_ASSERT_EQUAL_UINT16(300U, words[1]);
    uint8_t expected[WavinFrame::kMaxWriteRequestSize];
    TEST_ASSERT_EQUAL_UINT(8U, WavinFrame::buildRead(1U, 1U, 4U, 0U, 2U, expected, sizeof expected));
    TEST_ASSERT_EQUAL_UINT(1U, link.sent.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, link.sent[0].data(), 8U);

    // A write is acknowledged by the echo.
    const uint16_t value = 210U;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, client.writeRegisters(1U, 2U, 0U, 3U, &value, 1U));
    TEST_ASSERT_EQUAL_UINT(10U, WavinFrame::buildWrite(1U, 2U, 0U, 3U, &value, 1U, expected, sizeof expected));
    TEST_ASSERT_EQUAL_UINT(2U, link.sent.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, link.sent[1].data(), 10U);

    // Fewer registers than asked for are reported as received.
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, client.readRegisters(1U, 1U, 4U, 0U, 4U, words, 4U, received));
    TEST_ASSERT_EQUAL_UINT8(2U, received);

    // Requests the frame cannot carry are rejected before anything is sent.
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataValue,
                           client.readRegisters(1U, 1U, 4U, 0U, WavinFrame::kMaxRegisters + 1U, words, 4U,
                                                received));
    TEST_ASSERT_EQUAL_UINT8(0U, received);
    const uint16_t tooMany[WavinFrame::kMaxRegisters + 1U] = {};
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataValue,
                           client.writeRegisters(1U, 2U, 0U, 3U, tooMany, WavinFrame::kMaxRegisters + 1U));
    TEST_ASSERT_EQUAL_UINT(3U, link.sent.size());
}

void test_modbus_configuration_parses_wavin_datapoints(void) {
    ConfigurationRoot parsed;
    ModbusConfigDocument::ValidationError error;
    std::vector<ModbusConfigDocument::ParseNotice> notices;
    const auto parse = [&](const std::string &json) {
        return ModbusConfigDocument::parse(json.c_str(), json.size(), parsed, &error, &notices);
    };

    TEST_ASSERT_TRUE(parse(modbusConfigurationWithScale(0x43, "0.1", "int16", 1,
                                                        R"(,"category":1,"page":3)")));
    const ModbusDatapoint &read = parsed.devices[0].datapoints[0];
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WAVIN_READ), static_cast<int>(read.function));
    TEST_ASSERT_EQUAL_UINT16(10U, read.address);
    TEST_ASSERT_EQUAL_UINT8(1U, read.category);
    TEST_ASSERT_EQUAL_UINT8(3U, read.page);
    TEST_ASSERT_TRUE(isReadOnlyFunction(read.function));
    TEST_ASSERT_TRUE(notices.empty());

    TEST_ASSERT_TRUE(parse(modbusConfigurationWithScale(0x44, "0.1", "uint16", 2,
                                                        R"(,"category":2,"page":0)")));
    const ModbusDatapoint &write = parsed.devices[0].datapoints[0];
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WAVIN_WRITE), static_cast<int>(write.function));
    TEST_ASSERT_EQUAL_UINT8(1U, write.numOfRegisters);
    TEST_ASSERT_TRUE(isWriteFunction(write.function));
    TEST_ASSERT_TRUE(ModbusMqttWriteCore::isWritableHoldingFunction(write.function));

    // Wavin writes carry one 16-bit register, like FC6.
    TEST_ASSERT_FALSE(parse(modbusConfigurationWithScale(0x44, "1", "int32", 2,
                                                         R"(,"category":2,"page":0)")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusConfigDocument::ValidationReason::InvalidRegisterCount),
                          static_cast<int>(error.reason));

    // Reads are limited to what one reply carries.
    TEST_ASSERT_TRUE(parse(modbusConfigurationWithScale(0x43, "1", "uint16", 100,
                                                        R"(,"category":2,"page":0)")));
    TEST_ASSERT_EQUAL_UINT8(WavinFrame::kMaxRegisters, parsed.devices[0].datapoints[0].numOfRegisters);
    TEST_ASSERT_EQUAL_UINT(1U, notices.size());

    // Index, category and page are one byte each on the wire.
    const char *outOfRange[] = {R"(,"category":256,"page":0)", R"(,"category":1,"page":-1)"};
    for (const char *fields : outOfRange) {
        TEST_ASSERT_FALSE(parse(modbusConfigurationWithScale(0x43, "1", "uint16", 1, fields)));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusConfigDocument::ValidationReason::InvalidWavinAddress),
                              static_cast<int>(error.reason));
    }
    std::string wideIndex = modbusConfigurationWithScale(0x43, "1", "uint16", 1, R"(,"category":1,"page":0)");
    wideIndex.replace(wideIndex.find("\"address\":10"), 12, "\"address\":300");
    TEST_ASSERT_FALSE(parse(wideIndex));
    TEST_ASSERT_EQUAL_STRING("invalid_wavin_address",
                             ModbusConfigDocument::reasonToString(error.reason));

    // A location that is not an integer is rejected, not read as 0.
    const char *notIntegers[] = {R"(,"category":"2","page":0)", R"(,"category":1,"page":1.5)",
                                 R"(,"category":true,"page":0)"};
    for (const char *fields : notIntegers) {
        TEST_ASSERT_FALSE(parse(modbusConfigurationWithScale(0x43, "1", "uint16", 1, fields)));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusConfigDocument::ValidationReason::InvalidWavinAddress),
                              static_cast<int>(error.reason));
    }
    // Absent category and page mean 0.
    TEST_ASSERT_TRUE(parse(modbusConfigurationWithScale(0x43, "1", "uint16", 1, "")));
    TEST_ASSERT_EQUAL_UINT8(0U, parsed.devices[0].datapoints[0].category);
    TEST_ASSERT_EQUAL_UINT8(0U, parsed.devices[0].datapoints[0].page);

    // Standard functions ignore the Wavin fields.
    TEST_ASSERT_TRUE(parse(modbusConfigurationWithScale(3, "1", "uint16", 1, R"(,"category":999)")));
    TEST_ASSERT_EQUAL_UINT8(0U, parsed.devices[0].datapoints[0].category);
}

void test_modbus_configuration_rejects_baud_below_minimum(void) {
    ConfigurationRoot parsed;
    ModbusConfigDocument::ValidationError error;
    const auto parseWithBaud = [&](const char *baud) {
        std::string json = modbusConfigurationWithScale(3, "1");
        const std::string field = "\"baud\":9600";
        json.replace(json.find(field), field.size(), std::string("\"baud\":") + baud);
        return ModbusConfigDocument::parse(json.c_str(), json.size(), parsed, &error);
    };

    // The Wavin timing is checked at compile time against MODBUS_MIN_BAUD_RATE,
    // so no configuration may go below it; the bus form offers no lower rate.
    TEST_ASSERT_TRUE(parseWithBaud("1200"));
    TEST_ASSERT_EQUAL_INT(1200, parsed.bus.baud);
    TEST_ASSERT_TRUE(parseWithBaud("38400"));

    // Too low, or not an integer: a float, a string, a value outside the int
    // range or a boolean would otherwise fall back to the default and run the
    // bus at a rate the stored document does not show.
    const char *rejected[] = {"1199", "300", "0", "-9600", "9600.5", "\"9600\"", "4294967296", "true"};
    for (const char *baud : rejected) {
        TEST_ASSERT_FALSE_MESSAGE(parseWithBaud(baud), baud);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ModbusConfigDocument::ValidationReason::InvalidBaudRate),
                                      static_cast<int>(error.reason), baud);
    }
    TEST_ASSERT_EQUAL_STRING("invalid_baud_rate", ModbusConfigDocument::reasonToString(error.reason));
    TEST_ASSERT_EQUAL_STRING("Bus baud rate must be an integer of at least 1200",
                             ModbusConfigDocument::reasonDescription(error.reason));
    // Absent (null) keeps the default rate.
    TEST_ASSERT_TRUE(parseWithBaud("null"));
    TEST_ASSERT_EQUAL_INT(DEFAULT_MODBUS_BAUD_RATE, parsed.bus.baud);
    // The rejection concerns the bus, not a datapoint, so the API response
    // and the log carry no datapoint location for it.
    TEST_ASSERT_FALSE(ModbusConfigDocument::locatesDatapoint(error.reason));
    TEST_ASSERT_FALSE(ModbusConfigDocument::locatesDatapoint(ModbusConfigDocument::ValidationReason::InvalidJson));
    TEST_ASSERT_TRUE(ModbusConfigDocument::locatesDatapoint(
        ModbusConfigDocument::ValidationReason::InvalidWavinAddress));

    // An absent bus object keeps the default rate.
    const std::string noBus = R"({"version":1,"devices":[]})";
    TEST_ASSERT_TRUE(ModbusConfigDocument::parse(noBus.c_str(), noBus.size(), parsed, &error));
    TEST_ASSERT_EQUAL_INT(DEFAULT_MODBUS_BAUD_RATE, parsed.bus.baud);
}

void test_modbus_mqtt_wavin_write_carries_register_location(void) {
    ModbusMqttWriteCore::CommandContext context;
    context.slaveId = 1U;
    context.function = WAVIN_WRITE;
    context.address = 0U;
    context.category = 2U;
    context.page = 5U;
    context.scale = 0.1;
    context.dataType = INT16;

    std::vector<ModbusMqttWriteCore::WriteCommand> submissions;
    const auto handler = ModbusMqttWriteCore::buildMessageHandler(
        context,
        [&](const ModbusMqttWriteCore::WriteCommand &command) { submissions.push_back(command); },
        nullptr);
    handler("root/wavin/setpoint/set", "21.5");
    TEST_ASSERT_EQUAL_UINT(1U, submissions.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WAVIN_WRITE), static_cast<int>(submissions[0].function));
    TEST_ASSERT_EQUAL_UINT8(2U, submissions[0].category);
    TEST_ASSERT_EQUAL_UINT8(5U, submissions[0].page);
    TEST_ASSERT_EQUAL_UINT8(1U, submissions[0].wordCount);
    TEST_ASSERT_EQUAL_UINT16(215U, submissions[0].words[0]);

    // Like FC6, a Wavin write never encodes a multi-register type.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ModbusMqttWriteCore::RejectionReason::UnsupportedDataType),
                          static_cast<int>(ModbusMqttWriteCore::convertPayload(
                              WAVIN_WRITE, "1", 1.0, INT32, ModbusWordOrder::Big).rejection));
}

void test_protocol_profiles_bound_addresses_and_counts(void) {
    // Function codes map to the two built-in profiles; a code that names no
    // function is standard Modbus and is refused later as an illegal function.
    TEST_ASSERT_EQUAL_STRING("modbus-rtu", profileForFunction(3).id);
    TEST_ASSERT_EQUAL_STRING("modbus-rtu", profileForFunction(16).id);
    TEST_ASSERT_EQUAL_STRING("modbus-rtu", profileForFunction(0).id);
    TEST_ASSERT_EQUAL_STRING("modbus-rtu", profileForFunction(99).id);
    TEST_ASSERT_EQUAL_STRING("wavin-ahc9000", profileForFunction(0x43).id);
    TEST_ASSERT_EQUAL_STRING("wavin-ahc9000", profileForFunction(0x44).id);

    // Wavin: a one-byte index, so a run of words must end at index 255.
    const ProtocolProfile &wavin = kWavinAhc9000Profile;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, checkAddressRange(wavin, 255U, 1U));
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataAddress, checkAddressRange(wavin, 255U, 2U));
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataAddress, checkAddressRange(wavin, 256U, 1U));
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataAddress, checkAddressRange(wavin, 0xFFFFU, 1U));
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, checkAddressRange(wavin, 0U, 22U));
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, checkAddressRange(wavin, 234U, 22U));
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataAddress, checkAddressRange(wavin, 235U, 22U));
    // Count 0 checks the address alone, for a read whose length is rejected
    // on its own.
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, checkAddressRange(wavin, 255U, 0U));
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataAddress, checkAddressRange(wavin, 256U, 0U));

    // Standard Modbus: the whole 16-bit space.
    const ProtocolProfile &modbus = kModbusRtuProfile;
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, checkAddressRange(modbus, 0xFFFFU, 1U));
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataAddress, checkAddressRange(modbus, 0xFFFFU, 2U));
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, checkAddressRange(modbus, 0xFFC0U, 64U));
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataAddress, checkAddressRange(modbus, 0xFFC1U, 64U));
    TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataAddress, checkAddressRange(modbus, 0x10000U, 1U));

    // Register counts: 1..max per request; 0 and max + 1 are code 3. Wavin's
    // frame carries 22, ModbusMaster's buffers 64.
    const struct {
        const ProtocolProfile &profile;
        uint8_t max;
    } limits[] = {{wavin, 22U}, {modbus, 64U}};
    for (const auto &limit : limits) {
        TEST_ASSERT_EQUAL_UINT8(limit.max, limit.profile.maxReadRegisters);
        TEST_ASSERT_EQUAL_UINT8(limit.max, limit.profile.maxWriteRegisters);
        TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataValue, checkReadCount(limit.profile, 0U));
        TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, checkReadCount(limit.profile, 1U));
        TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, checkReadCount(limit.profile, limit.max));
        TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataValue, checkReadCount(limit.profile, limit.max + 1U));
        TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataValue, checkReadCount(limit.profile, 0xFFFFU));
        TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataValue, checkWriteCount(limit.profile, 0U));
        TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, checkWriteCount(limit.profile, 1U));
        TEST_ASSERT_EQUAL_HEX8(RtuFrame::kSuccess, checkWriteCount(limit.profile, limit.max));
        TEST_ASSERT_EQUAL_HEX8(RtuFrame::kIllegalDataValue,
                               checkWriteCount(limit.profile, static_cast<uint8_t>(limit.max + 1U)));
    }
}

int main(int /*argc*/, char ** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_single_chunk_returns_full_buffer);
    RUN_TEST(test_multi_chunk_assembles_in_order);
    RUN_TEST(test_interleaved_requests_are_independent);
    RUN_TEST(test_aborted_then_clean_request);
    RUN_TEST(test_lingering_slot_replaced_on_new_request);
    RUN_TEST(test_empty_body_returns_empty_buffer);
    RUN_TEST(test_post_oom_drains_safely);
    RUN_TEST(test_ota_password_length_policy);
    RUN_TEST(test_ota_password_record_set_verify_change_and_clear);
    RUN_TEST(test_ota_password_supported_iteration_bounds);
    RUN_TEST(test_ota_password_unsupported_legacy_cost_fails_without_deriving);
    RUN_TEST(test_ota_credential_service_persists_and_clears_record);
    RUN_TEST(test_reboot_scheduler_admits_once_and_executes_after_grace_period);
    RUN_TEST(test_reboot_scheduler_rolls_back_failed_task_admission);
    RUN_TEST(test_provisioning_attempt_saved_and_temporary_success_publish_coherent_status);
    RUN_TEST(test_provisioning_attempt_persistence_failures_never_become_ready);
    RUN_TEST(test_provisioning_attempt_tokens_ignore_stale_and_out_of_order_events);
    RUN_TEST(test_provisioning_event_barrier_drains_queued_old_events_before_replacement);
    RUN_TEST(test_provisioning_attempt_rejects_zero_and_malformed_station_addresses);
    RUN_TEST(test_wifi_credential_fields_accept_full_width_ssid_and_password);
    RUN_TEST(test_provisioning_attempt_cancel_and_failure_can_be_followed_by_success);
    RUN_TEST(test_wifi_configuration_storage_round_trips_static_dhcp_and_clear);
    RUN_TEST(test_wifi_configuration_storage_reports_write_clear_and_open_failures);
    RUN_TEST(test_provisioning_completion_requires_one_fully_ready_snapshot);
    RUN_TEST(test_provisioning_completion_maps_pending_and_unavailable_without_retrying);
    RUN_TEST(test_factory_reset_reports_success_only_after_all_steps_succeed);
    RUN_TEST(test_factory_reset_task_creation_failure_prevents_erasure);
    RUN_TEST(test_factory_reset_config_format_failure_is_reported);
    RUN_TEST(test_factory_reset_nvs_failure_is_reported);
    RUN_TEST(test_registered_ota_routes_decode_bearer_and_dispatch_first_upload_chunk);
    RUN_TEST(test_registered_factory_reset_route_clears_persisted_credential);
    RUN_TEST(test_registered_ota_routes_fail_closed_when_credential_storage_is_unavailable);
    RUN_TEST(test_mutation_request_guard_validates_marker_and_allowed_hosts);
    RUN_TEST(test_registered_mutation_matrix_fails_closed_before_dispatch);
    RUN_TEST(test_registered_ota_matrix_checks_mutation_context_before_auth_or_payload);
    RUN_TEST(test_registered_ota_routes_apply_context_session_and_busy_precedence_before_body);
    RUN_TEST(test_mqtt_connection_field_byte_boundaries);
    RUN_TEST(test_mqtt_multibyte_credentials_use_utf8_byte_lengths);
    RUN_TEST(test_mqtt_port_contract);
    RUN_TEST(test_mqtt_handler_core_rejects_before_storage_or_reload);
    RUN_TEST(test_mqtt_handler_core_accepts_boundary_values);
    RUN_TEST(test_mqtt_document_defaults_only_a_missing_port);
    RUN_TEST(test_mqtt_one_shot_test_restores_enabled_state_after_valid_load);
    RUN_TEST(test_invalid_persisted_mqtt_fields_clear_state_and_gate_client_calls);
    RUN_TEST(test_owner_mailbox_is_bounded_fifo_and_owns_value_payloads);
    RUN_TEST(test_owner_completion_is_exactly_once_and_cancels_only_before_start);
    RUN_TEST(test_owner_mailbox_reports_overload_unavailable_shutdown_and_fifo_replacement);
    RUN_TEST(test_mqtt_owner_core_enforces_context_generation_and_client_order);
    RUN_TEST(test_mqtt_owner_core_removal_during_dispatch_has_no_stale_invocation);
    RUN_TEST(test_mqtt_owner_queues_replacement_submitted_during_callback_dispatch);
    RUN_TEST(test_mqtt_owner_core_replacement_deduplicates_reconnect_subscriptions);
    RUN_TEST(test_mqtt_owner_quiesces_after_retained_signal_and_rejects_work_until_matching_resume);
    RUN_TEST(test_mqtt_owner_preserves_initially_disabled_state_across_ota);
    RUN_TEST(test_mqtt_owner_quiesce_is_bounded_when_signal_fails_or_broker_is_disconnected);
    RUN_TEST(test_modbus_owner_activates_only_after_transaction_boundary);
    RUN_TEST(test_modbus_owner_defers_activation_during_adhoc_command);
    RUN_TEST(test_modbus_owner_snapshots_remain_values_across_replacement);
    RUN_TEST(test_modbus_owner_quiesce_stops_commands_and_restores_only_prior_state);
    RUN_TEST(test_mqtt_owner_snapshots_remain_values_across_replacement);
    RUN_TEST(test_latest_revision_preserves_exact_candidate_across_overlapping_workers);
    RUN_TEST(test_configuration_revision_failure_is_inert_during_concurrent_worker_check);
    RUN_TEST(test_publication_attempt_tokens_ignore_stale_completions_after_retry);
    RUN_TEST(test_staged_bridge_plan_never_commits_after_running_timeout);
    RUN_TEST(test_async_publications_do_not_wait_for_a_stalled_owner);
    RUN_TEST(test_production_snapshot_core_never_returns_mixed_runtime_fields);
    RUN_TEST(test_modbus_mqtt_coil_payload_contract);
    RUN_TEST(test_modbus_mqtt_holding_payload_grammar_and_conversion);
    RUN_TEST(test_modbus_mqtt_typed_holding_conversion);
    RUN_TEST(test_modbus_mqtt_production_handler_submits_only_accepted_commands);
    RUN_TEST(test_modbus_configuration_rejects_noninvertible_writable_scales_atomically);
    RUN_TEST(test_modbus_configuration_coerces_register_shape_per_data_type);
    RUN_TEST(test_modbus_home_assistant_number_bounds_follow_scale_direction);
    RUN_TEST(test_modbus_command_topics_derive_from_the_state_base);
    RUN_TEST(test_modbus_codec_decodes_each_data_type);
    RUN_TEST(test_modbus_codec_encode_round_trips);
    RUN_TEST(test_modbus_codec_formats_decimals_from_scale);
    RUN_TEST(test_modbus_codec_formats_wide_integers_exactly);
    RUN_TEST(test_modbus_codec_formats_float32_with_shortest_round_trip);
    RUN_TEST(test_modbus_codec_format_reading_matches_publication);
    RUN_TEST(test_modbus_codec_parses_word_lists);
    RUN_TEST(test_ota_lifecycle_rejects_duplicates_and_retains_terminal_failure);
    RUN_TEST(test_ota_quiescence_deadlines_are_independent_and_rollover_safe);
    RUN_TEST(test_ota_progress_calculates_component_and_overall_without_overflow);
    RUN_TEST(test_ota_manual_progress_and_success_transition);
    RUN_TEST(test_ota_signed_single_component_reports_overall_progress);
    RUN_TEST(test_ota_heap_floor_requires_free_and_contiguous_headroom);
    RUN_TEST(test_ota_flash_boundary_floor_sits_below_quiescence_floor);
    RUN_TEST(test_ota_two_stage_admission_matches_observed_tls_cost);
    RUN_TEST(test_ota_manual_transfer_timeout_covers_start_idle_and_rollover);
    RUN_TEST(test_ota_sessions_enforce_capacity_expiry_revocation_and_hex_tokens);
    RUN_TEST(test_ota_progress_publisher_coalesces_and_applies_queue_heap_guards);
    RUN_TEST(test_ota_admission_gate_waits_for_existing_ui_lease_and_restores_selectively);
    RUN_TEST(test_ota_ui_filesystem_gate_prevents_access_until_safe_remount);
    RUN_TEST(test_rtu_frame_crc_matches_controller_replies);
    RUN_TEST(test_wavin_frame_builds_read_and_write_requests);
    RUN_TEST(test_wavin_frame_writes_whole_clock_page);
    RUN_TEST(test_rtu_frame_scans_replies_and_exceptions);
    RUN_TEST(test_rtu_frame_locates_reply_among_other_traffic);
    RUN_TEST(test_rtu_timing_scales_with_baud_and_fits_budget);
    RUN_TEST(test_rtu_transport_returns_the_reply_found_among_other_traffic);
    RUN_TEST(test_rtu_transport_retries_timeouts_and_crc_errors_within_the_budget);
    RUN_TEST(test_rtu_transport_exceptions_are_final_and_the_line_is_settled_after_a_failure);
    RUN_TEST(test_wavin_client_reads_and_writes_through_the_transport);
    RUN_TEST(test_modbus_configuration_parses_wavin_datapoints);
    RUN_TEST(test_modbus_configuration_rejects_baud_below_minimum);
    RUN_TEST(test_modbus_mqtt_wavin_write_carries_register_location);
    RUN_TEST(test_protocol_profiles_bound_addresses_and_counts);
    return UNITY_END();
}
