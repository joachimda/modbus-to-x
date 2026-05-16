// Native-host tests for BodyAccumulator (issue #005).
//
// BodyAccumulator has no Arduino dependencies, so we include its
// translation unit directly to avoid configuring test_build_src /
// build_src_filter just for one file.

#include "../src/network/mbx_server/BodyAccumulator.cpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unity.h>

namespace {

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

int main(int /*argc*/, char ** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_single_chunk_returns_full_buffer);
    RUN_TEST(test_multi_chunk_assembles_in_order);
    RUN_TEST(test_interleaved_requests_are_independent);
    RUN_TEST(test_aborted_then_clean_request);
    RUN_TEST(test_lingering_slot_replaced_on_new_request);
    RUN_TEST(test_empty_body_returns_empty_buffer);
    RUN_TEST(test_post_oom_drains_safely);
    return UNITY_END();
}
