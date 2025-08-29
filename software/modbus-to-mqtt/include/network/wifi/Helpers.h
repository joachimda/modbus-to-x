/**************************************************************
   Originally part of WiFiManager for the ESP8266/Arduino platform
   (https://github.com/tzapu/WiFiManager)
   Built by AlexT https://github.com/tzapu
   Ported to Async Web Server by https://github.com/alanswx
   Licensed under MIT license

   Modified by joachimda (2025)
   Changes include: [brief bullet list of major changes]
 **************************************************************/
#ifndef Helpers_h
#define Helpers_h

#include <WString.h>

using byte = uint8_t;

class Helpers {
public:
    static auto CalculateChipId() -> String;
private:
    static auto byteToHexString(const uint8_t *buf) -> String;

    // Unit Test Seam
    static uint64_t getEfuseMac();
};

#endif