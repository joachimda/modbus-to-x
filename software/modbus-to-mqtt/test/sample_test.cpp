// Native-host tests for BodyAccumulator (issue #005).
//
// BodyAccumulator has no Arduino dependencies, so we include its
// translation unit directly to avoid configuring test_build_src /
// build_src_filter just for one file.

#include "../src/network/mbx_server/BodyAccumulator.cpp"
#include "../include/network/mbx_server/OtaRouteRegistration.h"
#include "../include/services/FactoryResetCore.h"
#include "../src/services/ota/OtaCredentialCore.cpp"
#include "../src/services/ota/OtaAuthorizationCore.cpp"
#include "../src/services/ota/OtaCredentialService.cpp"
#include "../include/services/ota/OtaUploadGuardCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <Preferences.h>
#include <unity.h>

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
};

struct RouteTestState {
    int mode = 0;
    int unauthorizedCalls = 0;
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
};

struct RouteTestRequest {
    RouteTestMethod requestMethod;
    std::string requestUrl;
    std::string bearer;
    OtaUploadGuardCore::State uploadState{};
    RouteTestState *state = nullptr;

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
        if (route == nullptr || !route->uploadHandler) return false;
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

    bool cacheUploadAuthorization(RouteTestRequest *request) const {
        if (request == nullptr || request->uploadState.authorizationChecked) return false;
        bool authorized = authorizeRouteRequest(*request);
        return OtaUploadGuardCore::authorize(
            request->uploadState, returnAuthorizationResult, &authorized);
    }

    void sendUnauthorized(RouteTestRequest *) const {
        ++state->unauthorizedCalls;
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

        TEST_ASSERT_EQUAL_UINT(2U, server.count(Routes::OTA_FIRMWARE, ROUTE_TEST_POST));
        TEST_ASSERT_EQUAL_UINT(2U, server.count(Routes::OTA_FILESYSTEM, ROUTE_TEST_POST));
        TEST_ASSERT_EQUAL_UINT(2U, server.count(Routes::OTA_HTTP_CHECK, ROUTE_TEST_POST));
        TEST_ASSERT_EQUAL_UINT(2U, server.count(Routes::FACTORY_RESET, ROUTE_TEST_POST));

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
    RUN_TEST(test_factory_reset_reports_success_only_after_all_steps_succeed);
    RUN_TEST(test_factory_reset_task_creation_failure_prevents_erasure);
    RUN_TEST(test_factory_reset_config_format_failure_is_reported);
    RUN_TEST(test_factory_reset_nvs_failure_is_reported);
    RUN_TEST(test_registered_ota_routes_decode_bearer_and_dispatch_first_upload_chunk);
    RUN_TEST(test_registered_factory_reset_route_clears_persisted_credential);
    RUN_TEST(test_registered_ota_routes_fail_closed_when_credential_storage_is_unavailable);
    return UNITY_END();
}
