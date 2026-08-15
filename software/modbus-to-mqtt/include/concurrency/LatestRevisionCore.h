#ifndef MODBUS_TO_MQTT_LATEST_REVISION_CORE_H
#define MODBUS_TO_MQTT_LATEST_REVISION_CORE_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

// Separates request ordering from publication to an owner. issue() assigns an
// inert request-order ticket. An Admission holds the same lock used by readers
// while an owner-queue send is attempted, then publishes the ticket only after
// that send succeeds. A failed send therefore cannot transiently supersede an
// older admitted candidate.
class LatestRevisionCore {
public:
    class Admission {
    public:
        Admission(Admission &&other) noexcept
            : _owner(other._owner),
              _revision(other._revision),
              _lock(std::move(other._lock)) {
            other._owner = nullptr;
            other._revision = 0U;
        }

        Admission(const Admission &) = delete;
        Admission &operator=(const Admission &) = delete;
        Admission &operator=(Admission &&) = delete;

        uint32_t revision() const {
            return _revision;
        }

        void commit() {
            if (_owner == nullptr || !_lock.owns_lock()) return;
            if (_revision > _owner->_latest) _owner->_latest = _revision;
            _lock.unlock();
            _owner = nullptr;
        }

    private:
        friend class LatestRevisionCore;

        Admission(LatestRevisionCore &owner, const uint32_t revision)
            : _owner(&owner),
              _revision(revision),
              _lock(owner._mutex) {
        }

        LatestRevisionCore *_owner;
        uint32_t _revision;
        std::unique_lock<std::mutex> _lock;
    };

    uint32_t issue() {
        uint32_t revision = _next.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        if (revision != 0U) return revision;
        revision = _next.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        return revision;
    }

    Admission beginAdmission(const uint32_t revision) {
        return Admission(*this, revision);
    }

    bool isLatest(const uint32_t revision) const {
        const std::lock_guard<std::mutex> guard(_mutex);
        return revision != 0U && revision == _latest;
    }

    bool isObsolete(const uint32_t revision) const {
        const std::lock_guard<std::mutex> guard(_mutex);
        return revision != 0U && _latest > revision;
    }

    uint32_t latest() const {
        const std::lock_guard<std::mutex> guard(_mutex);
        return _latest;
    }

private:
    std::atomic<uint32_t> _next{0U};
    mutable std::mutex _mutex;
    uint32_t _latest{0U};
};

#endif
