#pragma once

#include <Arduino.h>
#ifndef AT24CDRIVER_H
#define AT24CDRIVER_H
class AT24CDriver {
  public:
    explicit AT24CDriver(const uint8_t deviceAddress = 0x50) {_deviceAddress = deviceAddress; };
    static bool begin();
    void writeByte(uint16_t memAddress, uint8_t data) const;
    uint8_t readByte(uint16_t memAddress) const;
    void writeBuffer(uint16_t memAddress, const uint8_t* data, uint16_t length) const;
    void readBuffer(uint16_t memAddress, uint8_t* buffer, uint16_t length) const;
  private:
    uint8_t _deviceAddress;
};

#endif //AT24CDRIVER_H
