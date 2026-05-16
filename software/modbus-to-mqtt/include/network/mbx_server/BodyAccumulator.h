#ifndef MODBUS_TO_MQTT_BODY_ACCUMULATOR_H
#define MODBUS_TO_MQTT_BODY_ACCUMULATOR_H

#include <cstddef>
#include <cstdint>

// Per-request body accumulator for ESPAsyncWebServer chunk-body handlers.
//
// ESPAsyncWebServer's AsyncWebServerRequest destructor frees _tempObject with
// free() (see WebRequest.cpp), so the slot must hold a malloc/calloc-allocated
// buffer — never a heap-allocated C++ object with a non-trivial destructor.
// This helper owns that lifecycle: it calloc()s into the slot on the first
// chunk and lets the framework free it when the request ends, whether the
// request completes normally or is aborted.
//
// The slot is taken by reference (void *&) so production code passes
// req->_tempObject directly, and tests pass a local void * variable.
//
// Returns a pointer to the (NUL-terminated) accumulated buffer on the final
// chunk. Returns nullptr while accumulation is still in progress, on OOM
// at index==0, or on any subsequent chunk after an OOM was observed.
//
// Caller must NOT free the returned pointer — the framework's request
// destructor (or the caller's test harness) owns it via the slot.
namespace BodyAccumulator {

char *append(void *&slot, const uint8_t *data, std::size_t len,
             std::size_t index, std::size_t total);

}  // namespace BodyAccumulator

#endif
