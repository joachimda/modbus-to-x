#ifndef MODBUS_TO_MQTT_RS485_PORT_H
#define MODBUS_TO_MQTT_RS485_PORT_H

#include <Arduino.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "modbus/RtuLink.h"
#include "modbus/config_structs/Bus.h"

class Logger;
class TeeStream;

// One RS-485 port: the UART with its serial settings, the transceiver's
// direction pin with its guard time, the lock that gives one transaction at
// a time the line, the receive capture for diagnostics and the error count.
// It carries bytes and knows nothing of the protocol: frames go out through
// send() and replies are read from stream() or, by RtuTransport, through
// the RtuLink interface. The current hardware has one port on one UART;
// select() is the hook for a revision with several.
class Rs485Port : public RtuLink {
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

    // RtuLink: the serial settings (the defaults until begin()), frames out,
    // bytes in, the clock and the random source for the retry backoff.
    uint32_t baudRate() const override;

    uint8_t bitsPerCharacter() const override;

    // Drives the transceiver, writes the frame, waits until it is on the
    // wire and releases the line again.
    void send(const uint8_t *data, size_t length) override;

    void discardInput() override;

    int read() override;

    uint32_t nowMs() const override;

    void idleMs(uint32_t ms) override;

    uint32_t randomWord() override;

    // Direction control for a caller that writes to stream() itself
    // (ModbusMaster's transmission callbacks): beginTransmit() before the
    // first byte, endTransmit() once the frame is written.
    void beginTransmit();

    void endTransmit();

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
