#ifndef MODBUS_TO_MQTT_COHERENT_SNAPSHOT_CORE_H
#define MODBUS_TO_MQTT_COHERENT_SNAPSHOT_CORE_H

#include <mutex>
#include <utility>

// A small value snapshot shared by one writer and arbitrary readers. Every
// field is copied while holding the same lock, preventing mixed generations.
template<typename Value>
class CoherentSnapshotCore {
public:
    Value read() const {
        const std::lock_guard<std::mutex> guard(_mutex);
        return _value;
    }

    void write(Value value) {
        const std::lock_guard<std::mutex> guard(_mutex);
        _value = std::move(value);
    }

private:
    mutable std::mutex _mutex;
    Value _value{};
};

#endif
