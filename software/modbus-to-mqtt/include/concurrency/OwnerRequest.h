#ifndef MODBUS_TO_MQTT_OWNER_REQUEST_H
#define MODBUS_TO_MQTT_OWNER_REQUEST_H

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "concurrency/OwnerCompletionCore.h"

// Cross-task requests are allocated as normal C++ objects and only their
// pointers are placed in FreeRTOS queues. This is important: FreeRTOS queues
// byte-copy items, so String, vector, function and other owning C++ types must
// never be stored in a queue item directly.
class OwnerRequest {
public:
    using State = OwnerCompletionCore::State;

    OwnerRequest()
        : _completion(xSemaphoreCreateBinary()) {
    }

    OwnerRequest(const OwnerRequest &) = delete;
    OwnerRequest &operator=(const OwnerRequest &) = delete;

    void retain() {
        _references.fetch_add(1U, std::memory_order_relaxed);
    }

    void release() {
        if (_references.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
            delete this;
        }
    }

    bool valid() const {
        return _completion != nullptr;
    }

    bool wait(const TickType_t timeoutTicks) const {
        if (_lifecycle.state() == State::Completed) return true;
        return _completion != nullptr && xSemaphoreTake(_completion, timeoutTicks) == pdTRUE;
    }

    bool markQueued() {
        return _lifecycle.markQueued();
    }

    bool tryStart() {
        return _lifecycle.tryStart();
    }

    bool cancelIfQueued() {
        return _lifecycle.cancelIfQueued();
    }

    State state() const {
        return _lifecycle.state();
    }

    void complete() {
        if (!_lifecycle.complete()) return;
        if (_completion != nullptr) xSemaphoreGive(_completion);
    }

protected:
    virtual ~OwnerRequest() {
        if (_completion != nullptr) vSemaphoreDelete(_completion);
    }

private:
    mutable SemaphoreHandle_t _completion{nullptr};
    std::atomic<uint16_t> _references{1U};
    OwnerCompletionCore _lifecycle;
};

#endif
