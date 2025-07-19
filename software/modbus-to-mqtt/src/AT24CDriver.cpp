#include "AT24CDriver.h"
#include <Config.h>
#include <Wire.h>

bool AT24CDriver::begin(){
  return Wire.begin();
}

void AT24CDriver::writeByte(const uint16_t memAddress, const uint8_t data) const {
  Wire.beginTransmission(_deviceAddress);
  Wire.write((memAddress >> 8) & 0xFF);
  Wire.write(memAddress & 0xFF);
  Wire.write(data);
  Wire.endTransmission();
  delay(EEPROM_WRITE_CYCLE_MS);
}

uint8_t AT24CDriver::readByte(const uint16_t memAddress) const {
  Wire.beginTransmission(_deviceAddress);
  Wire.write((memAddress >> 8) & 0xFF);
  Wire.write(memAddress & 0xFF);
  Wire.endTransmission();

  Wire.requestFrom(_deviceAddress, static_cast<uint8_t>(1));
  if (Wire.available()) {
    return Wire.read();
  }
  return 0xFF;
}

void AT24CDriver::writeBuffer(uint16_t memAddress, const uint8_t* data, const uint16_t length) const {
  uint16_t bytesWritten = 0;
  while (bytesWritten < length) {
    Wire.beginTransmission(_deviceAddress);
    Wire.write((memAddress >> 8) & 0xFF);
    Wire.write(memAddress & 0xFF);

    const uint8_t bytesToWrite = min(32 - 2, length - bytesWritten);
    for (uint8_t i = 0; i < bytesToWrite; i++) {
      Wire.write(data[bytesWritten + i]);
    }

    Wire.endTransmission();
    delay(EEPROM_WRITE_CYCLE_MS);
    memAddress += bytesToWrite;
    bytesWritten += bytesToWrite;
  }
}

void AT24CDriver::readBuffer(uint16_t memAddress, uint8_t* buffer, const uint16_t length) const {
  uint16_t bytesRead = 0;
  while (bytesRead < length) {
    Wire.beginTransmission(_deviceAddress);
    Wire.write((memAddress >> 8) & 0xFF);
    Wire.write(memAddress & 0xFF);
    Wire.endTransmission();

    const uint8_t chunkSize = min(32, length - bytesRead);
    Wire.requestFrom(_deviceAddress, chunkSize);

    for (uint8_t i = 0; i < chunkSize && Wire.available(); i++) {
      buffer[bytesRead + i] = Wire.read();
    }

    memAddress += chunkSize;
    bytesRead += chunkSize;
  }
}
