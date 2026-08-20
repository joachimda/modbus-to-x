#include "network/wifi/WifiConfigurationStorage.h"

#include <Preferences.h>
#include <cstring>

namespace {

constexpr char STORAGE_NAMESPACE[] = "mbx-network";
constexpr char STORAGE_KEY[] = "ipv4";
constexpr uint32_t RECORD_MAGIC = 0x4D425857U;
constexpr uint8_t RECORD_VERSION = 1U;
constexpr size_t IPV4_TEXT_CAPACITY = 16U;

struct Record {
    uint32_t magic = RECORD_MAGIC;
    uint8_t version = RECORD_VERSION;
    uint8_t useStatic = 0U;
    uint8_t reserved[2]{};
    char ip[IPV4_TEXT_CAPACITY]{};
    char gateway[IPV4_TEXT_CAPACITY]{};
    char subnet[IPV4_TEXT_CAPACITY]{};
    char dns1[IPV4_TEXT_CAPACITY]{};
    char dns2[IPV4_TEXT_CAPACITY]{};
};

bool copyText(const String &source, char (&target)[IPV4_TEXT_CAPACITY]) {
    if (source.length() >= IPV4_TEXT_CAPACITY) return false;
    std::memset(target, 0, sizeof(target));
    std::memcpy(target, source.c_str(), source.length());
    return true;
}

bool recordsEqual(const Record &left, const Record &right) {
    return std::memcmp(&left, &right, sizeof(Record)) == 0;
}

bool recordIsValid(const Record &record) {
    if (record.magic != RECORD_MAGIC || record.version != RECORD_VERSION || record.useStatic > 1U) return false;
    const char *values[] = {record.ip, record.gateway, record.subnet, record.dns1, record.dns2};
    for (const char *value : values) {
        if (std::memchr(value, '\0', IPV4_TEXT_CAPACITY) == nullptr) return false;
    }
    return record.useStatic == 0U
           || (record.ip[0] != '\0' && record.gateway[0] != '\0' && record.subnet[0] != '\0');
}

}  // namespace

bool WifiConfigurationStorage::save(const WifiStaticConfig &configuration) {
    Record record;
    record.useStatic = configuration.any() ? 1U : 0U;
    if (record.useStatic != 0U
        && (!copyText(configuration.ip, record.ip)
            || !copyText(configuration.gateway, record.gateway)
            || !copyText(configuration.subnet, record.subnet)
            || !copyText(configuration.dns1, record.dns1)
            || !copyText(configuration.dns2, record.dns2))) {
        return false;
    }

    Preferences preferences;
    if (!preferences.begin(STORAGE_NAMESPACE, false)) return false;
    const bool stored = preferences.putBytes(STORAGE_KEY, &record, sizeof(record)) == sizeof(record);
    Record readback{};
    const bool verified = stored
                          && preferences.getBytes(STORAGE_KEY, &readback, sizeof(readback)) == sizeof(readback)
                          && recordsEqual(record, readback);
    preferences.end();
    return verified;
}

WifiConfigurationStorage::LoadResult WifiConfigurationStorage::load(WifiStaticConfig &configuration) {
    configuration = {};
    Preferences preferences;
    // Opening read-write also distinguishes an absent namespace (normal for
    // devices upgraded from older firmware) from an unavailable NVS store.
    if (!preferences.begin(STORAGE_NAMESPACE, false)) return LoadResult::Unavailable;
    const size_t length = preferences.getBytesLength(STORAGE_KEY);
    if (length == 0U) {
        preferences.end();
        return LoadResult::NotFound;
    }
    Record record{};
    const bool loaded = length == sizeof(record)
                        && preferences.getBytes(STORAGE_KEY, &record, sizeof(record)) == sizeof(record);
    preferences.end();
    if (!loaded || !recordIsValid(record)) return LoadResult::Invalid;
    if (record.useStatic == 0U) return LoadResult::Dhcp;

    configuration.ip = record.ip;
    configuration.gateway = record.gateway;
    configuration.subnet = record.subnet;
    configuration.dns1 = record.dns1;
    configuration.dns2 = record.dns2;
    return LoadResult::Static;
}

bool WifiConfigurationStorage::clear() {
    Preferences preferences;
    if (!preferences.begin(STORAGE_NAMESPACE, false)) return false;
    const bool cleared = preferences.clear() && preferences.getBytesLength(STORAGE_KEY) == 0U;
    preferences.end();
    return cleared;
}
