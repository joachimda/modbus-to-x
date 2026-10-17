#ifndef MODBUS_TO_MQTT_RS485_PORT_H
#define MODBUS_TO_MQTT_RS485_PORT_H

#include <Arduino.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "modbus/config_structs/Bus.h"

class Logger;
class TeeStream;

// One RS-485 port: the UART with its serial settings, the transceiver's
// direction pin with its guard time, the lock that gives one transaction at
// a time the line, the receive capture for diagnostics and the error count.
// It carries bytes and knows nothing of the protocol: frames go out through
// send() and replies are read from stream(). The current hardware has one
// port on one UART; select() is the hook for a revision with several.
class Rs485Port {
public:
    // Exclusive use of the port for one transaction; false when the port is
    // taken. The port is released when the Guard goes out of scope.
    class Guard {
    public:
        Guard(Rs485Port &port, bool owns);
        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;
        Guard(Guard &&other) noexcept;
        Guard &operator=(Guard &&other) noexcept;
        ~Guard();
        explicit operator bool() const { return _owns; }
    private:
        Rs485Port *_port;
        bool _owns;
    };

    explicit Rs485Port(Logger *logger);

    // Configures the direction pin and the UART from the bus settings; may
    // be called again to reconfigure.
    bool begin(const Bus &busConfig);

    bool isInitialized() const;

    // Takes the port for one transaction and selects it on the transceiver.
    Guard acquire();

    // Routes the UART to this port. One port per UART on the current
    // hardware, so there is nothing to do yet.
    void select();

    // The UART, wrapped for receive capture.
    Stream &stream();

    // Serial settings the port runs with (the defaults until begin()), for
    // timing that depends on the wire speed. bitsPerCharacter() counts
    // start, data, parity and stop bits: 10 for 8N1, 12 for 8E2.
    uint32_t baudRate() const;

    uint8_t bitsPerCharacter() const;

    // Drives the transceiver, writes the frame, waits until it is on the
    // wire and releases the line again.
    void send(const uint8_t *data, size_t length);

    // Direction control for a caller that writes to stream() itself
    // (ModbusMaster's transmission callbacks): beginTransmit() before the
    // first byte, endTransmit() once the frame is written.
    void beginTransmit();

    void endTransmit();

    // Throws away the bytes waiting in the receive buffer.
    void discardInput();

    // Discards input until the line has been quiet for quietMs since the
    // last byte, counting from lastByteAtMs (the millis() of the last byte
    // the caller saw), for at most maxMs.
    void waitForQuietLine(uint32_t lastByteAtMs, uint32_t quietMs, uint32_t maxMs);

    void incrementError();

    uint32_t errorCount() const;

    // Hex dump of the bytes captured since capture was last enabled.
    String dumpRx() const;

    void enableCapture(bool enable);

private:
    void release();

    Logger *_logger;
    TeeStream *_tee{nullptr};
    std::atomic<bool> _initialized{false};
    std::atomic<bool> _busy{false};
    std::atomic<uint32_t> _errorCount{0};
    uint32_t _baud;
    uint8_t _bitsPerCharacter;
};

#endif
