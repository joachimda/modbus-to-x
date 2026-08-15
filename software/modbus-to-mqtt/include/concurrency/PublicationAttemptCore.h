#ifndef MODBUS_TO_MQTT_PUBLICATION_ATTEMPT_CORE_H
#define MODBUS_TO_MQTT_PUBLICATION_ATTEMPT_CORE_H

#include <cstdint>

// Owner-local token used to distinguish callbacks from abandoned publication
// attempts. Invalidating an attempt before retry makes every already queued or
// concurrently arriving completion from the old attempt harmless.
class PublicationAttemptCore {
public:
    uint32_t begin() {
        advance();
        return _current;
    }

    void invalidate() {
        advance();
    }

    uint32_t current() const {
        return _current;
    }

    bool accepts(const uint32_t attempt) const {
        return attempt != 0U && attempt == _current;
    }

private:
    void advance() {
        ++_current;
        if (_current == 0U) ++_current;
    }

    uint32_t _current{0U};
};

#endif
