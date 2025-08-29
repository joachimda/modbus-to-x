/**************************************************************
    Originally part of WiFiManager for the ESP8266/Arduino platform
   (https://github.com/tzapu/WiFiManager)
   Built by AlexT https://github.com/tzapu
   Ported to Async Web Server by https://github.com/alanswx
   Licensed under MIT license

   Modified by joachimda (2025)
   Changes include: simplification to accommodate custom use
 **************************************************************/
#include "network/wifi/AsyncWiFiManagerParameter.h"
#include <cstring>
#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCUnusedGlobalDeclarationInspection"
AsyncWiFiManagerParameter::AsyncWiFiManagerParameter(const char *custom)
{
    _id = nullptr;
    _placeholder = nullptr;
    _length = 0;
    _value = nullptr;

    _customHTML = custom;
}

AsyncWiFiManagerParameter::AsyncWiFiManagerParameter(const char *id,
                                                     const char *placeholder,
                                                     const char *defaultValue,
                                                     unsigned int length)
{
    init(id, placeholder, defaultValue, length, "");
}

AsyncWiFiManagerParameter::AsyncWiFiManagerParameter(const char *id,
                                                     const char *placeholder,
                                                     const char *defaultValue,
                                                     unsigned int length,
                                                     const char *custom)
{
    init(id, placeholder, defaultValue, length, custom);
}

void AsyncWiFiManagerParameter::init(const char *id,
                                     const char *placeholder,
                                     const char *defaultValue,
                                     unsigned int length,
                                     const char *custom)
{
    _id = id;
    _placeholder = placeholder;
    _length = length;
    _value = new char[length + 1];

    for (unsigned int i = 0; i < length; i++)
    {
        _value[i] = 0;
    }
    if (defaultValue != nullptr)
    {
        strncpy(_value, defaultValue, length);
    }

    _customHTML = custom;
}

const char *AsyncWiFiManagerParameter::getValue()
{
    return _value;
}

const char *AsyncWiFiManagerParameter::getID()
{
    return _id;
}

const char *AsyncWiFiManagerParameter::getPlaceholder()
{
    return _placeholder;
}

unsigned int AsyncWiFiManagerParameter::getValueLength() const
{
    return _length;
}

const char *AsyncWiFiManagerParameter::getCustomHTML()
{
    return _customHTML;
}

#pragma clang diagnostic pop