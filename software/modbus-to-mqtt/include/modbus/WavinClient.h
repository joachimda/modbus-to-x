#ifndef MODBUS_TO_MQTT_WAVIN_CLIENT_H
#define MODBUS_TO_MQTT_WAVIN_CLIENT_H

#include <cstddef>
#include <cstdint>

class RtuLink;

// Wavin AHC 9000 transactions (0x43 read, 0x44 write) over the shared RS-485
// port. ModbusMaster only speaks the standard function codes, so these
// frames are built and decoded here (see WavinFrame) and carried by
// RtuTransport with the Wavin retry policy from Config.h. The caller must
// hold the port's guard. Returns a ModbusMaster-compatible status code.
class WavinClient {
public:
    explicit WavinClient(RtuLink &link);

    // On success receivedCount holds the number of registers the reply
    // carried, which may be fewer than count.
    uint8_t readRegisters(uint8_t slave, uint8_t category, uint8_t index, uint8_t page,
                          uint8_t count, uint16_t *words, size_t wordCapacity,
                          uint8_t &receivedCount);

    uint8_t writeRegisters(uint8_t slave, uint8_t category, uint8_t index, uint8_t page,
                           const uint16_t *words, uint8_t count);

private:
    // Runs the request through the transport and decodes the reply's
    // registers. replyWords bounds the reply the scanner accepts and the
    // timeout allows for: the count asked for by a read, kMaxRegisters for
    // a write, whose reply may echo any count.
    uint8_t exchange(const uint8_t *request, size_t requestLength, uint8_t slave,
                     uint8_t function, uint8_t replyWords, uint8_t attempts, uint16_t *words,
                     size_t wordCapacity, uint8_t &receivedCount);

    RtuLink &_link;
};

#endif
