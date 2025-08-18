/**************************************************************
    Originally part of WiFiManager for the ESP8266/Arduino platform
   (https://github.com/tzapu/WiFiManager)
   Built by AlexT https://github.com/tzapu
   Ported to Async Web Server by https://github.com/alanswx
   Licensed under MIT license

   Modified by joachimda (2025)
   Changes include: simplification to accommodate custom use
 **************************************************************/

#ifndef MODBUS_TO_MQTT_ASYNCWIFIMANAGERPARAMETER_H
#define MODBUS_TO_MQTT_ASYNCWIFIMANAGERPARAMETER_H

class AsyncWiFiManagerParameter {
public:
    explicit AsyncWiFiManagerParameter(const char *custom);
    AsyncWiFiManagerParameter(const char *id,
                              const char *placeholder,
                              const char *defaultValue,
                              unsigned int length);
    AsyncWiFiManagerParameter(const char *id,
                              const char *placeholder,
                              const char *defaultValue,
                              unsigned int length,
                              const char *custom);

    const char *getID();
    const char *getValue();
    const char *getPlaceholder();
    unsigned int getValueLength() const;
    const char *getCustomHTML();

private:
    const char *_id;
    const char *_placeholder;
    char *_value;
    unsigned int _length;
    const char *_customHTML;

    void init(const char *id,
              const char *placeholder,
              const char *defaultValue,
              unsigned int length,
              const char *custom);

    friend class AsyncWiFiManager;
};

#endif
