#ifndef TEE_STREAM_H
#define TEE_STREAM_H

#include <Arduino.h>
#include <Stream.h>
#include "Logger.h"

class TeeStream : public Stream {
public:
    explicit TeeStream(Stream &inner, Logger *logger);

    void enableCapture(bool en);
    String dumpHex() const;

    // Stream interface
    int available() override;
    int read() override;
    int peek() override;
    void flush() override;
    size_t write(uint8_t ch) override;
    size_t write(const uint8_t *buffer, size_t size) override;

private:
    Stream &_inner;
    Logger *_logger;
    bool _capture{false};
    uint8_t _buf[64]{};
    size_t _bufLen{0};
    bool _sawFirstByte{false};
};

#endif // TEE_STREAM_H
