#include "modbus/ProtocolProfile.h"

#include "modbus/ModbusFunctionUtils.h"
#include "modbus/RtuFrame.h"

const ProtocolProfile &profileForFunction(const int code) {
    return isWavinFunctionCode(code) ? kWavinAhc9000Profile : kModbusRtuProfile;
}

uint8_t checkAddressRange(const ProtocolProfile &profile, const uint32_t address, const uint16_t count) {
    if (address > profile.maxAddress) return RtuFrame::kIllegalDataAddress;
    // Compared as distances so the sum cannot wrap.
    if (count > 1U && count - 1U > profile.maxAddress - address) return RtuFrame::kIllegalDataAddress;
    return RtuFrame::kSuccess;
}

uint8_t checkReadCount(const ProtocolProfile &profile, const uint16_t count) {
    return count == 0U || count > profile.maxReadRegisters ? RtuFrame::kIllegalDataValue
                                                           : RtuFrame::kSuccess;
}

uint8_t checkWriteCount(const ProtocolProfile &profile, const uint8_t count) {
    return count == 0U || count > profile.maxWriteRegisters ? RtuFrame::kIllegalDataValue
                                                            : RtuFrame::kSuccess;
}
