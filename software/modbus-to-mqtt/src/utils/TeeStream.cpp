#include "Config.h"
#include "utils/TeeStream.h"

TeeStream::TeeStream(Stream &inner, Logger *logger)
    : _inner(inner), _logger(logger) {}

void TeeStream::enableCapture(const bool en) {
    _capture = en;
    if (en) {
        _bufLen = 0;
        _sawFirstByte = false;
    }
}

String TeeStream::dumpHex() const {
    if (_bufLen == 0) return {""};
    String s;
    s.reserve(_bufLen * 3 + 8);
    s = " RX=";
    for (size_t i = 0; i < _bufLen; ++i) {
        if (_buf[i] < 16) s += "0";
        s += String(_buf[i], HEX);
        if (i + 1 < _bufLen) s += " ";
    }
    return s;
}

int TeeStream::available() {
#if RS485_DROP_LEADING_ZERO
    if (_capture && !_sawFirstByte) {
        // Non-blocking purge of leading 0x00 bytes
        while (_inner.available() > 0) {
            const int pk = _inner.peek();
            if (pk != 0x00) break;
            // Drop zero
            (void) _inner.read();
            // Do not record in the capture buffer and do not set _sawFirstByte
        }
    }
#endif
    return _inner.available();
}

int TeeStream::read() {
    int b = _inner.read();
#if RS485_DROP_LEADING_ZERO
    if (_capture && !_sawFirstByte) {
        // Drop leading 0x00 bytes; if the next byte isn't immediately available,
        // wait briefly for it to arrive to avoid returning a spurious 0x00.
        uint32_t waited = 0;
        int drops = 0;
        while (b == 0x00 && drops < 8) {
            if (_inner.available() > 0) {
                b = _inner.read();
                drops++;
                continue;
            }
            if (waited >= RS485_FIRSTBYTE_WAIT_US) {
                // Do not propagate a zero as the first byte; report "no data"
                return -1;
            }
            delayMicroseconds(20);
            waited += 20;
        }
    }
#endif
    if (_capture && b >= 0 && _bufLen < sizeof(_buf)) {
        _buf[_bufLen++] = static_cast<uint8_t>(b);
        if (!_sawFirstByte && b != 0x00) {
            _sawFirstByte = true;
        }
    } else if (_capture && !_sawFirstByte && b == 0x00) {
        // Explicitly ignore zero as the first byte for state tracking
    }
    return b;
}

int TeeStream::peek() {
#if RS485_DROP_LEADING_ZERO
    if (_capture && !_sawFirstByte) {
        // Purge any leading zeros so peek exposes the first non-zero
        while (_inner.available() > 0) {
            const int pk = _inner.peek();
            if (pk != 0x00) break;
            (void) _inner.read(); // drop zero
        }
    }
#endif
    return _inner.peek();
}

void TeeStream::flush() { _inner.flush(); }

size_t TeeStream::write(const uint8_t ch) { return _inner.write(ch); }

size_t TeeStream::write(const uint8_t *buffer, const size_t size) { return _inner.write(buffer, size); }
