#ifndef MODBUS_TO_MQTT_OWNER_MAILBOX_CORE_H
#define MODBUS_TO_MQTT_OWNER_MAILBOX_CORE_H

#include <array>
#include <cstddef>

// Platform-neutral bounded FIFO used to exercise the mailbox admission and
// ordering contract in native tests. Firmware uses a FreeRTOS queue with the
// same fixed-capacity/no-overwrite semantics.
template<typename Item, size_t Capacity>
class OwnerMailboxCore {
public:
    static_assert(Capacity > 0U, "Owner mailbox capacity must be positive");

    enum class Admission {
        Accepted,
        Full,
        Unavailable,
        Shutdown,
    };

    Admission admit(const Item &item) {
        if (_shutdown) return Admission::Shutdown;
        if (!_available) return Admission::Unavailable;
        if (_size == Capacity) return Admission::Full;
        _items[_tail] = item;
        _tail = (_tail + 1U) % Capacity;
        ++_size;
        return Admission::Accepted;
    }

    bool tryPush(const Item &item) {
        return admit(item) == Admission::Accepted;
    }

    bool tryPop(Item &item) {
        if (_size == 0U) return false;
        item = _items[_head];
        _head = (_head + 1U) % Capacity;
        --_size;
        return true;
    }

    size_t size() const { return _size; }
    constexpr size_t capacity() const { return Capacity; }

    void setAvailable(const bool available) { _available = available; }
    void shutdown() { _shutdown = true; }

private:
    std::array<Item, Capacity> _items{};
    size_t _head{0U};
    size_t _tail{0U};
    size_t _size{0U};
    bool _available{true};
    bool _shutdown{false};
};

#endif
