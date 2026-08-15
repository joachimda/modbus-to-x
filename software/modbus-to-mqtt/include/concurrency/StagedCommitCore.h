#ifndef MODBUS_TO_MQTT_STAGED_COMMIT_CORE_H
#define MODBUS_TO_MQTT_STAGED_COMMIT_CORE_H

#include <atomic>
#include <cstdint>
#include <utility>

// The owner may stage a candidate without making it live. A second task then
// publishes an approval token only after its side of the coordinated commit is
// complete. A timed-out staging request is harmless because it is never
// approved and therefore can never replace the active value.
template<typename Value>
class StagedCommitCore {
public:
    void stage(Value value, const uint32_t token) {
        _stagedValue = std::move(value);
        _stagedToken = token;
        _hasStaged = true;
    }

    void approve(const uint32_t token) {
        _approvedToken.store(token, std::memory_order_release);
    }

    bool takeApproved(Value &value, uint32_t &token) {
        if (!_hasStaged || _approvedToken.load(std::memory_order_acquire) != _stagedToken) {
            return false;
        }
        value = std::move(_stagedValue);
        token = _stagedToken;
        _hasStaged = false;
        return true;
    }

    uint32_t stagedToken() const {
        return _hasStaged ? _stagedToken : 0U;
    }

private:
    Value _stagedValue{};
    uint32_t _stagedToken{0U};
    bool _hasStaged{false};
    std::atomic<uint32_t> _approvedToken{0U};
};

#endif
