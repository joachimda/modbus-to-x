#include "network/mbx_server/BodyAccumulator.h"

#include <cstdlib>
#include <cstring>

namespace BodyAccumulator {

char *append(void *&slot, const uint8_t *data, const std::size_t len,
             const std::size_t index, const std::size_t total) {
    if (index == 0U) {
        if (slot != nullptr) {
            // Defensive: a prior aborted request that wasn't cleaned up by
            // the framework. Drop it before allocating a fresh buffer.
            std::free(slot);
            slot = nullptr;
        }
        slot = std::calloc(total + 1U, sizeof(char));
        if (slot == nullptr) return nullptr;
    }

    if (slot == nullptr) {
        // OOM was observed at index==0; keep draining chunks but produce
        // no buffer. The handler's final-chunk branch must check for
        // nullptr and respond with 500.
        return nullptr;
    }

    if (len > 0U && data != nullptr) {
        // Guard against the framework over-delivering past `total` by
        // clamping; the +1 NUL byte must remain untouched.
        const std::size_t copy_end = index + len;
        const std::size_t safe_end = copy_end > total ? total : copy_end;
        if (safe_end > index) {
            std::memcpy(static_cast<char *>(slot) + index, data, safe_end - index);
        }
    }

    if (index + len < total) return nullptr;
    return static_cast<char *>(slot);
}

}  // namespace BodyAccumulator
