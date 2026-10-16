#ifndef MODBUS_TO_MQTT_RTU_LINK_H
#define MODBUS_TO_MQTT_RTU_LINK_H

#include <cstddef>
#include <cstdint>

// What RtuTransport needs from the platform: the RS-485 line, the clock and
// a source of randomness. Rs485Port implements it on the device; the native
// tests drive the transport through a scripted fake. No Arduino types, so
// the transport compiles on the host.
class RtuLink {
public:
    virtual ~RtuLink() {
    }

    // Serial settings the line runs with; bitsPerCharacter() counts start,
    // data, parity and stop bits.
    virtual uint32_t baudRate() const = 0;

    virtual uint8_t bitsPerCharacter() const = 0;

    // Writes a frame and returns once it is on the wire.
    virtual void send(const uint8_t *data, size_t length) = 0;

    // Throws away the bytes waiting in the receive buffer.
    virtual void discardInput() = 0;

    // The next received byte, or -1 when none is waiting.
    virtual int read() = 0;

    // Milliseconds since start; wraps like millis().
    virtual uint32_t nowMs() const = 0;

    // Gives the time away for ms milliseconds.
    virtual void idleMs(uint32_t ms) = 0;

    // A random 32-bit value, for the retry backoff.
    virtual uint32_t randomWord() = 0;
};

#endif
