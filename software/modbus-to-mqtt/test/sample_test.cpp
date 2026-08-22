// Native-host tests for BodyAccumulator (issue #005).
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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <Preferences.h>
#include <unity.h>

#include "../src/utils/StringUtils.cpp"
#include "../src/modbus/ModbusConfigDocument.cpp"

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
};

struct RouteTestRequest {
    RouteTestMethod requestMethod;
    std::string requestUrl;
    std::string bearer;
    OtaUploadGuardCore::State uploadState{};
    RouteTestState *state = nullptr;
    std::string marker = MutationRequestGuardCore::REQUEST_HEADER_VALUE;
    std::string host = "192.168.1.42";

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
        return request != nullptr && authorizeRouteRequest(*request);
    }

    bool isMutationAllowed(const RouteTestRequest *request) const {
        return request != nullptr && MutationRequestGuardCore::isAllowed(
            request->marker.c_str(), request->host.c_str(), "192.168.1.42", "192.168.4.1", "modbus-to-x");
    }

    bool cacheUploadAuthorization(RouteTestRequest *request) const {
        if (request == nullptr || request->uploadState.authorizationChecked) return false;
        bool authorized = authorizeRouteRequest(*request);
        return OtaUploadGuardCore::authorize(
            request->uploadState, returnAuthorizationResult, &authorized);
    }

    void sendUnauthorized(RouteTestRequest *) const {
        ++state->unauthorizedCalls;
    }

    void sendForbidden(RouteTestRequest *) const {
        ++state->forbiddenCalls;
    }

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
        }
    }

    void handleDeletePassword(RouteTestRequest *) const {
        ++state->deletePasswordCalls;
        (void)OtaCredentialService::clearPassword();
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
    constexpr const char *wrongBearer = "d3JvbmctcGFzc3dvcmQ=";
    const char *uploadRoutes[] = {Routes::OTA_FIRMWARE, Routes::OTA_FILESYSTEM};

    for (int routeMode = 0; routeMode < 2; ++routeMode) {
        Preferences::resetTestStorage();
        RouteTestState state;
        state.mode = routeMode;
        RouteTestServer server = configuredRouteTestServer(state);

        TEST_ASSERT_EQUAL_UINT(3U, server.count(Routes::OTA_FIRMWARE, ROUTE_TEST_POST));
        TEST_ASSERT_EQUAL_UINT(3U, server.count(Routes::OTA_FILESYSTEM, ROUTE_TEST_POST));
        TEST_ASSERT_EQUAL_UINT(3U, server.count(Routes::OTA_HTTP_CHECK, ROUTE_TEST_POST));
        TEST_ASSERT_EQUAL_UINT(3U, server.count(Routes::FACTORY_RESET, ROUTE_TEST_POST));

        RouteTestRequest setPassword{ROUTE_TEST_PUT, Routes::OTA_PASSWORD, "", {}, &state};
        TEST_ASSERT_TRUE(server.dispatchBody(setPassword, R"({"password":"route-password"})"));
        TEST_ASSERT_EQUAL_INT(1, state.passwordHandlerCalls);
        TEST_ASSERT_TRUE(OtaCredentialService::isProtected());

        RouteTestRequest absentCheck{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, "", {}, &state};
        TEST_ASSERT_TRUE(server.dispatchRequest(absentCheck));
        TEST_ASSERT_EQUAL_INT(1, state.unauthorizedCalls);
        TEST_ASSERT_EQUAL_INT(0, state.checkCalls);

        RouteTestRequest malformedCheck{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, "%%%not-base64%%%", {}, &state};
        TEST_ASSERT_TRUE(server.dispatchRequest(malformedCheck));
        TEST_ASSERT_EQUAL_INT(2, state.unauthorizedCalls);
        TEST_ASSERT_EQUAL_INT(0, state.checkCalls);

        RouteTestRequest wrongCheck{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, wrongBearer, {}, &state};
        TEST_ASSERT_TRUE(server.dispatchRequest(wrongCheck));
        TEST_ASSERT_EQUAL_INT(3, state.unauthorizedCalls);
        TEST_ASSERT_EQUAL_INT(0, state.checkCalls);

        RouteTestRequest correctCheck{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, correctBearer, {}, &state};
        TEST_ASSERT_TRUE(server.dispatchRequest(correctCheck));
        TEST_ASSERT_EQUAL_INT(1, state.checkCalls);

        RouteTestRequest correctNotes{ROUTE_TEST_POST, Routes::OTA_HTTP_NOTES, correctBearer, {}, &state};
        RouteTestRequest correctApply{ROUTE_TEST_POST, Routes::OTA_HTTP_APPLY, correctBearer, {}, &state};
        TEST_ASSERT_TRUE(server.dispatchRequest(correctNotes));
        TEST_ASSERT_TRUE(server.dispatchRequest(correctApply));
        TEST_ASSERT_EQUAL_INT(1, state.notesCalls);
        TEST_ASSERT_EQUAL_INT(1, state.applyCalls);

        for (int imageType = 0; imageType < 2; ++imageType) {
            const int beginCallsBefore = state.firmwareBeginCalls + state.filesystemBeginCalls;
            const int writesBefore = state.writeCalls;
            uint8_t firstChunk[] = {1U, 2U, 3U};

            RouteTestRequest absent{ROUTE_TEST_POST, uploadRoutes[imageType], "", {}, &state};
            TEST_ASSERT_TRUE(server.dispatchUploadChunk(absent, firstChunk, sizeof(firstChunk)));
            RouteTestRequest wrong{ROUTE_TEST_POST, uploadRoutes[imageType], wrongBearer, {}, &state};
            TEST_ASSERT_TRUE(server.dispatchUploadChunk(wrong, firstChunk, sizeof(firstChunk)));
            TEST_ASSERT_EQUAL_INT(beginCallsBefore, state.firmwareBeginCalls + state.filesystemBeginCalls);
            TEST_ASSERT_EQUAL_INT(writesBefore, state.writeCalls);

            RouteTestRequest correct{ROUTE_TEST_POST, uploadRoutes[imageType], correctBearer, {}, &state};
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

    RouteTestRequest setPassword{ROUTE_TEST_PUT, Routes::OTA_PASSWORD, "", {}, &state};
    TEST_ASSERT_TRUE(server.dispatchBody(setPassword, R"({"password":"route-password"})"));
    TEST_ASSERT_TRUE(OtaCredentialService::isProtected());

    RouteTestRequest rejectedReset{ROUTE_TEST_POST, Routes::FACTORY_RESET, "", {}, &state};
    TEST_ASSERT_TRUE(server.dispatchBody(rejectedReset, R"({"confirm":"factory-reset"})"));
    TEST_ASSERT_EQUAL_INT(1, state.unauthorizedCalls);
    TEST_ASSERT_EQUAL_INT(0, state.factoryResetHandlerCalls);
    TEST_ASSERT_TRUE(OtaCredentialService::isProtected());

    RouteTestRequest acceptedReset{ROUTE_TEST_POST, Routes::FACTORY_RESET, correctBearer, {}, &state};
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

        TEST_ASSERT_EQUAL_INT(8, state.unauthorizedCalls);
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
            TEST_ASSERT_EQUAL_UINT(2U, server.count(route.path, route.method));

            const int callsBefore = state.mutationSideEffectCalls;
            RouteTestRequest valid{route.method, route.path, "", {}, &state};
            TEST_ASSERT_TRUE(route.hasBody ? server.dispatchBody(valid, "{}") : server.dispatchRequest(valid));
            TEST_ASSERT_EQUAL_INT(callsBefore + 1, state.mutationSideEffectCalls);

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
    RouteTestRequest accepted{ROUTE_TEST_POST, Routes::OTA_HTTP_CHECK, correctBearer, {}, &state};
    accepted.host = "MODBUS-TO-X.LOCAL:80";
    TEST_ASSERT_TRUE(server.dispatchRequest(accepted));
    TEST_ASSERT_EQUAL_INT(1, state.checkCalls);

    RouteTestRequest readSettings{ROUTE_TEST_GET, Routes::OTA_HTTP_SETTINGS, "", {}, &state};
    readSettings.marker.clear();
    readSettings.host = "public.example";
    TEST_ASSERT_TRUE(server.dispatchRequest(readSettings));
    TEST_ASSERT_EQUAL_INT(1, state.getSettingsCalls);
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

void test_modbus_owner_activates_only_after_complete_polling_pass(void) {
    RecordingModbusOwnerBus bus;
    ModbusOwnerCore<ModbusOwnerTestConfiguration, RecordingModbusOwnerBus> owner(bus);
    owner.requestActivation({1, 9600, 3U, 6U});
    TEST_ASSERT_TRUE(owner.processBoundary());

    std::vector<int> observedIdentities;
    owner.runPollingPass([&](const ModbusOwnerTestConfiguration &active, const size_t deviceIndex) {
        observedIdentities.push_back(active.identity);
        if (deviceIndex == 1U) {
            owner.requestActivation({2, 19200, 2U, 4U});
            TEST_ASSERT_FALSE(owner.processBoundary());
        }
    });
    TEST_ASSERT_EQUAL_UINT(3U, observedIdentities.size());
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

void test_modbus_mqtt_production_handler_submits_only_accepted_commands(void) {
    ModbusMqttWriteCore::CommandContext context;
    context.slaveId = 7U;
    context.function = WRITE_MULTIPLE_HOLDING;
    context.address = 321U;
    context.registerCount = 8U;
    context.scale = 1.0;
    context.modbusGeneration = 42U;

    std::vector<ModbusMqttWriteCore::WriteCommand> submissions;
    std::vector<std::string> warnings;
    const auto handler = ModbusMqttWriteCore::buildMessageHandler(
        context,
        [&](const ModbusMqttWriteCore::WriteCommand &command) { submissions.push_back(command); },
        [&](const std::string &warning) { warnings.push_back(warning); });

    handler("root/device/write", "secret-invalid-payload");
    TEST_ASSERT_TRUE(submissions.empty());
    TEST_ASSERT_EQUAL_UINT(1U, warnings.size());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[0].find("topic=root/device/write"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[0].find("function=16"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[0].find("address=321"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[0].find("reason=invalid_syntax"));
    TEST_ASSERT_EQUAL(std::string::npos, warnings[0].find("secret-invalid-payload"));

    handler("root/device/write", "-1e-9999");
    handler("root/device/write", "65535.0000000000000001");
    TEST_ASSERT_TRUE(submissions.empty());
    TEST_ASSERT_EQUAL_UINT(3U, warnings.size());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[1].find("reason=out_of_range"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, warnings[2].find("reason=out_of_range"));
    TEST_ASSERT_EQUAL(std::string::npos, warnings[1].find("-1e-9999"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      warnings[2].find("65535.0000000000000001"));

    handler("root/device/write", "10");
    TEST_ASSERT_EQUAL_UINT(1U, submissions.size());
    TEST_ASSERT_EQUAL_STRING("root/device/write", submissions[0].sourceTopic.c_str());
    TEST_ASSERT_EQUAL_UINT8(7U, submissions[0].slaveId);
    TEST_ASSERT_EQUAL_INT(16, static_cast<int>(submissions[0].function));
    TEST_ASSERT_EQUAL_UINT16(321U, submissions[0].address);
    TEST_ASSERT_EQUAL_UINT8(1U, submissions[0].registerCount);
    TEST_ASSERT_EQUAL_UINT16(10U, submissions[0].rawValue);
    TEST_ASSERT_EQUAL_UINT32(42U, submissions[0].modbusGeneration);

    handler("root/device/write", "65536");
    handler("root/device/write", "1");
    TEST_ASSERT_EQUAL_UINT(2U, submissions.size());
    TEST_ASSERT_EQUAL_UINT16(1U, submissions[1].rawValue);
    TEST_ASSERT_EQUAL_UINT(4U, warnings.size());
}

std::string modbusConfigurationWithScale(const int function, const char *scale) {
    return std::string(R"({"version":1,"bus":{"enabled":true,"baud":9600,"serialFormat":"8N1"},"devices":[{"id":"device","name":"Device","slaveId":1,"dataPoints":[{"id":"point","name":"Point","function":)")
           + std::to_string(function)
           + R"(,"address":10,"numOfRegisters":1,"scale":)" + scale
           + R"(,"dataType":"uint16","unit":"","topic":"write"}]}]})";
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
    const auto positive = ModbusMqttWriteCore::numberDiscoveryBounds(positiveScale);
    TEST_ASSERT_TRUE(positive.valid());
    TEST_ASSERT_TRUE(std::fabs(positive.minimum) < 0.0000001);
    TEST_ASSERT_TRUE(std::fabs(positive.maximum - (65535.0 * positiveScale)) < 0.000001);
    TEST_ASSERT_TRUE(std::fabs(positive.step - positiveScale) < 0.0000001);

    const double negativeScale = static_cast<double>(-0.25F);
    const auto negative = ModbusMqttWriteCore::numberDiscoveryBounds(negativeScale);
    TEST_ASSERT_TRUE(negative.valid());
    TEST_ASSERT_TRUE(std::fabs(negative.minimum - (65535.0 * negativeScale)) < 0.000001);
    TEST_ASSERT_TRUE(std::fabs(negative.maximum) < 0.0000001);
    TEST_ASSERT_TRUE(std::fabs(negative.step - 0.25) < 0.0000001);

    TEST_ASSERT_FALSE(ModbusMqttWriteCore::numberDiscoveryBounds(0.0).valid());
    TEST_ASSERT_FALSE(ModbusMqttWriteCore::numberDiscoveryBounds(INFINITY).valid());
    TEST_ASSERT_FALSE(ModbusMqttWriteCore::numberDiscoveryBounds(NAN).valid());
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
    RUN_TEST(test_modbus_owner_activates_only_after_complete_polling_pass);
    RUN_TEST(test_modbus_owner_defers_activation_during_adhoc_command);
    RUN_TEST(test_modbus_owner_snapshots_remain_values_across_replacement);
    RUN_TEST(test_mqtt_owner_snapshots_remain_values_across_replacement);
    RUN_TEST(test_latest_revision_preserves_exact_candidate_across_overlapping_workers);
    RUN_TEST(test_configuration_revision_failure_is_inert_during_concurrent_worker_check);
    RUN_TEST(test_publication_attempt_tokens_ignore_stale_completions_after_retry);
    RUN_TEST(test_staged_bridge_plan_never_commits_after_running_timeout);
    RUN_TEST(test_async_publications_do_not_wait_for_a_stalled_owner);
    RUN_TEST(test_production_snapshot_core_never_returns_mixed_runtime_fields);
    RUN_TEST(test_modbus_mqtt_coil_payload_contract);
    RUN_TEST(test_modbus_mqtt_holding_payload_grammar_and_conversion);
    RUN_TEST(test_modbus_mqtt_production_handler_submits_only_accepted_commands);
    RUN_TEST(test_modbus_configuration_rejects_noninvertible_writable_scales_atomically);
    RUN_TEST(test_modbus_home_assistant_number_bounds_follow_scale_direction);
    return UNITY_END();
}
