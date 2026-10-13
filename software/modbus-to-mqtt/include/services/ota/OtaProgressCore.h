#ifndef MODBUS_TO_MQTT_OTA_PROGRESS_CORE_H
#define MODBUS_TO_MQTT_OTA_PROGRESS_CORE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace OtaProgressCore {

constexpr size_t TARGET_VERSION_CAPACITY = 32U;
constexpr size_t ERROR_CODE_CAPACITY = 48U;

enum class State : uint8_t {
    Idle = 0,
    Quiescing,
    AwaitingUpload,
    DownloadingWritingFirmware,
    VerifyingFirmware,
    DownloadingWritingFilesystem,
    VerifyingFilesystem,
    FailureDetected,
    Resuming,
    Failed,
    Rebooting,
};

enum class Component : uint8_t {
    None = 0,
    Firmware,
    Filesystem,
};

enum class TerminalResult : uint8_t {
    None = 0,
    Failed,
    Rebooting,
};

struct Snapshot {
    uint32_t operationId{0U};
    uint32_t sequence{0U};
    char targetVersion[TARGET_VERSION_CAPACITY]{};
    State state{State::Idle};
    Component component{Component::None};
    TerminalResult result{TerminalResult::None};
    uint32_t receivedBytes{0U};
    uint32_t totalBytes{0U};
    uint8_t componentPercent{0U};
    uint8_t overallPercent{0U};
    bool componentPercentKnown{false};
    bool overallPercentKnown{false};
    char errorCode[ERROR_CODE_CAPACITY]{};
};

inline const char *stateName(const State state) {
    switch (state) {
        case State::Idle: return "idle";
        case State::Quiescing: return "quiescing";
        case State::AwaitingUpload: return "awaiting_upload";
        case State::DownloadingWritingFirmware: return "downloading_writing_firmware";
        case State::VerifyingFirmware: return "verifying_firmware";
        case State::DownloadingWritingFilesystem: return "downloading_writing_filesystem";
        case State::VerifyingFilesystem: return "verifying_filesystem";
        case State::FailureDetected: return "failure_detected";
        case State::Resuming: return "resuming";
        case State::Failed: return "failed";
        case State::Rebooting: return "rebooting";
    }
    return "idle";
}

inline const char *componentName(const Component component) {
    switch (component) {
        case Component::None: return "none";
        case Component::Firmware: return "firmware";
        case Component::Filesystem: return "filesystem";
    }
    return "none";
}

inline const char *resultName(const TerminalResult result) {
    switch (result) {
        case TerminalResult::None: return "none";
        case TerminalResult::Failed: return "failed";
        case TerminalResult::Rebooting: return "rebooting";
    }
    return "none";
}

inline uint8_t percentage(const uint32_t received, const uint32_t total) {
    if (total == 0U) return 0U;
    const uint32_t bounded = std::min(received, total);
    return static_cast<uint8_t>((static_cast<uint64_t>(bounded) * 100U) / total);
}

inline bool meetsHeapFloor(const uint32_t freeHeap, const uint32_t largestBlock,
                           const uint32_t freeHeapFloor, const uint32_t largestBlockFloor) {
    return freeHeap >= freeHeapFloor && largestBlock >= largestBlockFloor;
}

inline bool manualTransferTimedOut(const State state, const bool timerArmed,
                                   const uint32_t now, const uint32_t lastActivity,
                                   const uint32_t timeout) {
    const bool awaitingTransfer = state == State::AwaitingUpload
                               || state == State::DownloadingWritingFirmware
                               || state == State::DownloadingWritingFilesystem;
    return timerArmed && awaitingTransfer
        && static_cast<uint32_t>(now - lastActivity) >= timeout;
}

inline void copyFixed(char *destination, const size_t capacity, const char *source) {
    if (destination == nullptr || capacity == 0U) return;
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    std::strncpy(destination, source, capacity - 1U);
    destination[capacity - 1U] = '\0';
}

class Lifecycle {
public:
    enum class Admission : uint8_t {
        Accepted = 0,
        Busy,
    };

    Admission reserve(const char *targetVersion, const bool firmwareSelected,
                      const bool filesystemSelected, const bool manualSingleComponent) {
        if (_active) return Admission::Busy;
        _active = true;
        _manualSingleComponent = manualSingleComponent;
        _firmwareSelected = firmwareSelected;
        _filesystemSelected = filesystemSelected;
        _firmwareReceived = 0U;
        _firmwareTotal = 0U;
        _filesystemReceived = 0U;
        _filesystemTotal = 0U;
        _firmwareTotalKnown = false;
        _filesystemTotalKnown = false;
        ++_nextOperationId;
        if (_nextOperationId == 0U) ++_nextOperationId;

        Snapshot next;
        next.operationId = _nextOperationId;
        next.sequence = _snapshot.sequence + 1U;
        if (next.sequence == 0U) ++next.sequence;
        next.state = State::Quiescing;
        copyFixed(next.targetVersion, sizeof(next.targetVersion), targetVersion);
        _snapshot = next;
        return Admission::Accepted;
    }

    bool markAwaitingUpload(const uint32_t operationId) {
        return transition(operationId, State::AwaitingUpload, Component::None);
    }

    bool beginComponent(const uint32_t operationId, const Component component,
                        const uint32_t totalBytes) {
        if (!matchesActive(operationId) || component == Component::None) return false;
        const bool allowed = _snapshot.state == State::Quiescing
                          || _snapshot.state == State::AwaitingUpload
                          || _snapshot.state == State::VerifyingFirmware;
        if (!allowed) return false;
        if (component == Component::Firmware && !_firmwareSelected) return false;
        if (component == Component::Filesystem && !_filesystemSelected) return false;

        if (component == Component::Firmware) {
            _firmwareTotal = totalBytes;
            _firmwareTotalKnown = totalBytes > 0U;
            _firmwareReceived = 0U;
        } else {
            _filesystemTotal = totalBytes;
            _filesystemTotalKnown = totalBytes > 0U;
            _filesystemReceived = 0U;
        }
        _snapshot.state = component == Component::Firmware
                              ? State::DownloadingWritingFirmware
                              : State::DownloadingWritingFilesystem;
        _snapshot.component = component;
        _snapshot.result = TerminalResult::None;
        _snapshot.receivedBytes = 0U;
        _snapshot.totalBytes = totalBytes;
        _snapshot.componentPercentKnown = totalBytes > 0U;
        _snapshot.componentPercent = 0U;
        _snapshot.errorCode[0] = '\0';
        updateOverall();
        bumpSequence();
        return true;
    }

    bool updateBytes(const uint32_t operationId, const uint32_t receivedBytes) {
        if (!matchesActive(operationId)) return false;
        const Component component = _snapshot.component;
        if (component == Component::None
            || (_snapshot.state != State::DownloadingWritingFirmware
                && _snapshot.state != State::DownloadingWritingFilesystem)) return false;
        const uint32_t total = component == Component::Firmware ? _firmwareTotal : _filesystemTotal;
        const uint32_t bounded = total > 0U ? std::min(receivedBytes, total) : receivedBytes;
        if (component == Component::Firmware) _firmwareReceived = bounded;
        else _filesystemReceived = bounded;
        _snapshot.receivedBytes = bounded;
        _snapshot.totalBytes = total;
        _snapshot.componentPercentKnown = total > 0U;
        _snapshot.componentPercent = percentage(bounded, total);
        updateOverall();
        bumpSequence();
        return true;
    }

    bool beginVerification(const uint32_t operationId, const Component component) {
        if (!matchesActive(operationId) || component != _snapshot.component) return false;
        const State expected = component == Component::Firmware
                                   ? State::DownloadingWritingFirmware
                                   : State::DownloadingWritingFilesystem;
        if (_snapshot.state != expected) return false;
        _snapshot.state = component == Component::Firmware
                              ? State::VerifyingFirmware
                              : State::VerifyingFilesystem;
        bumpSequence();
        return true;
    }

    bool completeComponent(const uint32_t operationId, const Component component) {
        if (!matchesActive(operationId) || component != _snapshot.component) return false;
        const State expected = component == Component::Firmware
                                   ? State::VerifyingFirmware
                                   : State::VerifyingFilesystem;
        if (_snapshot.state != expected) return false;
        if (component == Component::Firmware && _firmwareTotalKnown) {
            _firmwareReceived = _firmwareTotal;
        } else if (component == Component::Filesystem && _filesystemTotalKnown) {
            _filesystemReceived = _filesystemTotal;
        }
        updateOverall();
        bumpSequence();
        return true;
    }

    bool failureDetected(const uint32_t operationId, const char *errorCode) {
        if (!matchesActive(operationId)) return false;
        _snapshot.state = State::FailureDetected;
        _snapshot.result = TerminalResult::None;
        copyFixed(_snapshot.errorCode, sizeof(_snapshot.errorCode), errorCode);
        bumpSequence();
        return true;
    }

    bool markResuming(const uint32_t operationId) {
        if (!matchesActive(operationId) || _snapshot.state != State::FailureDetected) return false;
        _snapshot.state = State::Resuming;
        bumpSequence();
        return true;
    }

    bool markFailed(const uint32_t operationId, const char *errorCode) {
        if (!matchesActive(operationId)
            || (_snapshot.state != State::FailureDetected && _snapshot.state != State::Resuming)) return false;
        _snapshot.state = State::Failed;
        _snapshot.component = Component::None;
        _snapshot.result = TerminalResult::Failed;
        copyFixed(_snapshot.errorCode, sizeof(_snapshot.errorCode), errorCode);
        _active = false;
        bumpSequence();
        return true;
    }

    bool markRebooting(const uint32_t operationId) {
        if (!matchesActive(operationId)) return false;
        const bool valid = _snapshot.state == State::VerifyingFirmware
                        || _snapshot.state == State::VerifyingFilesystem
                        || _snapshot.state == State::AwaitingUpload;
        if (!valid) return false;
        _snapshot.state = State::Rebooting;
        _snapshot.component = Component::None;
        _snapshot.result = TerminalResult::Rebooting;
        _snapshot.errorCode[0] = '\0';
        bumpSequence();
        return true;
    }

    bool active() const { return _active; }
    uint32_t operationId() const { return _snapshot.operationId; }
    const Snapshot &snapshot() const { return _snapshot; }

private:
    bool matchesActive(const uint32_t operationId) const {
        return _active && operationId != 0U && operationId == _snapshot.operationId;
    }

    bool transition(const uint32_t operationId, const State state, const Component component) {
        if (!matchesActive(operationId) || _snapshot.state != State::Quiescing) return false;
        _snapshot.state = state;
        _snapshot.component = component;
        bumpSequence();
        return true;
    }

    void updateOverall() {
        bool known = false;
        uint64_t received = 0U;
        uint64_t total = 0U;
        const bool allSelectedTotalsKnown = (!_firmwareSelected || _firmwareTotalKnown)
                                         && (!_filesystemSelected || _filesystemTotalKnown);
        if (allSelectedTotalsKnown && (_firmwareSelected || _filesystemSelected)) {
            known = true;
            if (_firmwareSelected) {
                received += _firmwareReceived;
                total += _firmwareTotal;
            }
            if (_filesystemSelected) {
                received += _filesystemReceived;
                total += _filesystemTotal;
            }
        }
        _snapshot.overallPercentKnown = known && total > 0U;
        _snapshot.overallPercent = _snapshot.overallPercentKnown
                                       ? static_cast<uint8_t>((std::min(received, total) * 100U) / total)
                                       : 0U;
    }

    void bumpSequence() {
        ++_snapshot.sequence;
        if (_snapshot.sequence == 0U) ++_snapshot.sequence;
    }

    Snapshot _snapshot{};
    uint32_t _nextOperationId{0U};
    uint32_t _firmwareReceived{0U};
    uint32_t _firmwareTotal{0U};
    uint32_t _filesystemReceived{0U};
    uint32_t _filesystemTotal{0U};
    bool _firmwareSelected{false};
    bool _filesystemSelected{false};
    bool _firmwareTotalKnown{false};
    bool _filesystemTotalKnown{false};
    bool _manualSingleComponent{false};
    bool _active{false};
};

class Publisher {
public:
    bool shouldPublish(const Snapshot &snapshot, const uint32_t now,
                       const size_t clientCount, const size_t pendingMessages,
                       const uint32_t freeHeap, const uint32_t largestBlock,
                       const uint32_t heapFloor, const uint32_t blockFloor,
                       const bool force = false) {
        if (clientCount == 0U || pendingMessages != 0U
            || freeHeap < heapFloor || largestBlock < blockFloor) return false;
        if (!force && snapshot.sequence == _lastSequence) return false;

        const bool meaningful = snapshot.state != _lastState
                             || snapshot.result != _lastResult
                             || snapshot.componentPercent != _lastPercent;
        const uint32_t elapsed = now - _lastPublishedAt;
        if (!force && elapsed < (meaningful ? 100U : 1000U)) return false;

        _lastSequence = snapshot.sequence;
        _lastState = snapshot.state;
        _lastResult = snapshot.result;
        _lastPercent = snapshot.componentPercent;
        _lastPublishedAt = now;
        return true;
    }

private:
    uint32_t _lastSequence{0U};
    uint32_t _lastPublishedAt{0U};
    State _lastState{State::Idle};
    TerminalResult _lastResult{TerminalResult::None};
    uint8_t _lastPercent{0U};
};

}  // namespace OtaProgressCore

#endif
