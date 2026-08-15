#include "modbus/ModbusRuntimeTypes.h"

const char *modbusOperationResultToString(const ModbusOperationResult result) {
    switch (result) {
        case ModbusOperationResult::Success: return "success";
        case ModbusOperationResult::QueueFull: return "queue_full";
        case ModbusOperationResult::Timeout: return "timeout";
        case ModbusOperationResult::Unavailable: return "unavailable";
        case ModbusOperationResult::Shutdown: return "shutdown";
        case ModbusOperationResult::StaleGeneration: return "stale_generation";
        case ModbusOperationResult::ActivationFailed: return "activation_failed";
        case ModbusOperationResult::Superseded: return "superseded";
    }
    return "unknown";
}
