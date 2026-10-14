#ifndef MODBUS_TO_MQTT_WAVIN_CLIENT_H
#define MODBUS_TO_MQTT_WAVIN_CLIENT_H

#include <cstddef>
#include <cstdint>

class ModbusBus;

// Wavin AHC 9000 transactions (0x43 read, 0x44 write) over the shared RS-485
// bus. ModbusMaster only speaks the standard function codes, so these frames
// are written and parsed here (see WavinFrame). The caller must hold the bus
// guard. Returns a ModbusMaster-compatible status code.
class WavinClient {
public:
    explicit WavinClient(ModbusBus &bus);

    // On success receivedCount holds the number of registers the reply
    // carried, which may be fewer than count.
    uint8_t readRegisters(uint8_t slave, uint8_t category, uint8_t index, uint8_t page,
                          uint8_t count, uint16_t *words, size_t wordCapacity,
                          uint8_t &receivedCount);

    uint8_t writeRegisters(uint8_t slave, uint8_t category, uint8_t index, uint8_t page,
                           const uint16_t *words, uint8_t count);

private:
    // Timeouts and CRC errors (typically a collision with another master);
    // exception replies from the controller are final.
    static bool isRetryable(uint8_t status);

    uint8_t transact(const uint8_t *request, size_t requestLength, uint8_t slave,
                     uint8_t function, uint8_t expectedWords, uint16_t *words,
                     size_t wordCapacity, uint8_t &receivedCount);

    ModbusBus &_bus;
};

#endif
