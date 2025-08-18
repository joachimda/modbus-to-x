/**************************************************************
    Originally part of WiFiManager for the ESP8266/Arduino platform
   (https://github.com/tzapu/WiFiManager)
   Built by AlexT https://github.com/tzapu
   Ported to Async Web Server by https://github.com/alanswx
   Licensed under MIT license

   Modified by joachimda (2025)
   Changes include:
    - simplification to accommodate custom use.
    - removal of multi-device compatibility
 **************************************************************/

#include "Helpers.h"
static const auto mac_len = 6;
static auto hexCharArray = "0123456789ABCDEF";
static const auto hex_radix = 16;
static const auto hexUpper = 0xFF;
static const auto bits = 8;

/**
* convert char array (hex values) to readable string by separator
* buf:           buffer to convert
* length:        data length
* return:        formated value as String
*/
auto Helpers::byteToHexString(const uint8_t *buf) -> String
{
    String dataString = "";
    for (uint8_t i = 0; i < mac_len; i++)
    {
        byte v = buf[i] / hex_radix;
        byte w = buf[i] % hex_radix;
        if (i > 0)
        {
            dataString += "";
        }
        dataString += String(hexCharArray[v]);
        dataString += String(hexCharArray[w]);
    }
    dataString.toUpperCase();
    return dataString;
}


auto Helpers::CalculateChipId() -> String
{
    const uint64_t mac = getEfuseMac();
    std::array<uint8_t, mac_len> bytes{};

    for (int i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = static_cast<uint8_t>((mac >> (bits * (sizeof(bytes) - 1 - i))) & hexUpper);
    }
    return byteToHexString(bytes.data());
}

// Unit Test Seam
__attribute__((weak)) auto Helpers::getEfuseMac() -> uint64_t {
    return ESP.getEfuseMac();
}