#ifndef MODBUS_TO_MQTT_PROTOCOL_PROFILE_H
#define MODBUS_TO_MQTT_PROTOCOL_PROFILE_H

#include <cstdint>

#include "modbus/WavinFrame.h"

// What a protocol on the RS-485 bus allows per request. The generic layers
// (command execution, the HTTP and MQTT paths) check addresses and register
// counts against the device's profile instead of knowing any protocol's
// limits; the limits themselves stay with the protocol. Fields are added
// when a caller needs them.
struct ProtocolProfile {
    const char *id;
    // Highest register address a request may name: a flat register number
    // for Modbus, the index within a category/page for Wavin.
    uint16_t maxAddress;
    // Registers one read or one write request may carry.
    uint8_t maxReadRegisters;
    uint8_t maxWriteRegisters;
};

// Standard Modbus RTU through ModbusMaster, whose response and transmit
// buffers hold 64 registers (ku8MaxBufferSize); the protocol itself allows
// 125 per read and 123 per write.
constexpr ProtocolProfile kModbusRtuProfile = {"modbus-rtu", 0xFFFFU, 64U, 64U};
// Wavin AHC 9000: a one-byte register index, 1..22 registers per 0x43/0x44.
constexpr ProtocolProfile kWavinAhc9000Profile = {"wavin-ahc9000", 0xFFU, WavinFrame::kMaxRegisters,
                                                  WavinFrame::kMaxRegisters};

// The profile a function code belongs to: 0x43/0x44 are Wavin, every other
// code standard Modbus. The only mapping from a function code to a
// protocol; a protocol field per device replaces its callers.
const ProtocolProfile &profileForFunction(int code);

// Request checks, pure so they are tested natively. Each returns the Modbus
// exception code the request earns (RtuFrame::kIllegalDataAddress or
// RtuFrame::kIllegalDataValue) or RtuFrame::kSuccess, so a request no device
// would accept is answered without touching the bus.

// Registers address .. address + count - 1 must lie within the address
// space; count 0 checks the address alone.
uint8_t checkAddressRange(const ProtocolProfile &profile, uint32_t address, uint16_t count);
// 1..maxReadRegisters.
uint8_t checkReadCount(const ProtocolProfile &profile, uint16_t count);
// 1..maxWriteRegisters.
uint8_t checkWriteCount(const ProtocolProfile &profile, uint8_t count);

#endif
