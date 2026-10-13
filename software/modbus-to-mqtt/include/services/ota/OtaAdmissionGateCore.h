#ifndef MODBUS_TO_MQTT_OTA_ADMISSION_GATE_CORE_H
#define MODBUS_TO_MQTT_OTA_ADMISSION_GATE_CORE_H

#include <cstddef>

namespace OtaAdmissionGateCore {

class Gate {
public:
    void close() {
        _ordinaryMutationOpen = false;
        _runtimeJobsOpen = false;
        _uiFilesystemOpen = false;
        _normalEventsOpen = false;
    }

    void reopen(const bool uiFilesystemMounted) {
        _ordinaryMutationOpen = true;
        _runtimeJobsOpen = true;
        _uiFilesystemOpen = uiFilesystemMounted;
        _normalEventsOpen = true;
    }

    bool tryAcquireUiLease() {
        if (!_uiFilesystemOpen) return false;
        ++_uiLeases;
        return true;
    }

    void releaseUiLease() {
        if (_uiLeases > 0U) --_uiLeases;
    }

    bool ordinaryMutationOpen() const { return _ordinaryMutationOpen; }
    bool runtimeJobsOpen() const { return _runtimeJobsOpen; }
    bool uiFilesystemOpen() const { return _uiFilesystemOpen; }
    bool normalEventsOpen() const { return _normalEventsOpen; }
    size_t uiLeaseCount() const { return _uiLeases; }

private:
    bool _ordinaryMutationOpen{true};
    bool _runtimeJobsOpen{true};
    bool _uiFilesystemOpen{true};
    bool _normalEventsOpen{true};
    size_t _uiLeases{0U};
};

}  // namespace OtaAdmissionGateCore

#endif
