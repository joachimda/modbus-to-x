#ifndef MODBUS_TO_MQTT_TEST_PREFERENCES_H
#define MODBUS_TO_MQTT_TEST_PREFERENCES_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

class Preferences {
public:
    bool begin(const char *name, const bool readOnly = false) {
        if (name == nullptr || beginFailure) return false;
        namespaceName = name;
        this->readOnly = readOnly;
        open = true;
        return true;
    }

    void end() {
        open = false;
    }

    bool getBool(const char *key, const bool defaultValue = false) const {
        const auto *value = find(key);
        return value != nullptr && value->size() == sizeof(bool) ? (*value)[0] != 0U : defaultValue;
    }

    uint8_t getUChar(const char *key, const uint8_t defaultValue = 0) const {
        const auto *value = find(key);
        return value != nullptr && value->size() == sizeof(uint8_t) ? (*value)[0] : defaultValue;
    }

    uint32_t getUInt(const char *key, const uint32_t defaultValue = 0) const {
        const auto *value = find(key);
        uint32_t result = defaultValue;
        if (value != nullptr && value->size() == sizeof(result)) std::memcpy(&result, value->data(), sizeof(result));
        return result;
    }

    size_t getBytesLength(const char *key) const {
        const auto *value = find(key);
        return value == nullptr ? 0U : value->size();
    }

    size_t getBytes(const char *key, void *buffer, const size_t capacity) const {
        const auto *value = find(key);
        if (value == nullptr || buffer == nullptr || capacity < value->size()) return 0U;
        std::memcpy(buffer, value->data(), value->size());
        return value->size();
    }

    size_t putBool(const char *key, const bool value) {
        const uint8_t stored = value ? 1U : 0U;
        return putBytes(key, &stored, sizeof(stored));
    }

    size_t putUChar(const char *key, const uint8_t value) {
        return putBytes(key, &value, sizeof(value));
    }

    size_t putUInt(const char *key, const uint32_t value) {
        return putBytes(key, &value, sizeof(value));
    }

    size_t putBytes(const char *key, const void *value, const size_t length) {
        if (!open || readOnly || key == nullptr || value == nullptr) return 0U;
        auto &stored = values[namespaceName][key];
        const auto *bytes = static_cast<const uint8_t *>(value);
        stored.assign(bytes, bytes + length);
        return length;
    }

    bool clear() {
        if (!open || readOnly) return false;
        values[namespaceName].clear();
        return true;
    }

    static void resetTestStorage() {
        values.clear();
        beginFailure = false;
    }

    static void setBeginFailure(const bool fail) {
        beginFailure = fail;
    }

private:
    using Namespace = std::unordered_map<std::string, std::vector<uint8_t>>;

    const std::vector<uint8_t> *find(const char *key) const {
        if (!open || key == nullptr) return nullptr;
        const auto namespaceIt = values.find(namespaceName);
        if (namespaceIt == values.end()) return nullptr;
        const auto valueIt = namespaceIt->second.find(key);
        return valueIt == namespaceIt->second.end() ? nullptr : &valueIt->second;
    }

    inline static std::unordered_map<std::string, Namespace> values;
    inline static bool beginFailure = false;
    std::string namespaceName;
    bool readOnly = false;
    bool open = false;
};

#endif
