#ifndef MODBUS_TO_MQTT_ARDUINOOTAMANAGER_H
#define MODBUS_TO_MQTT_ARDUINOOTAMANAGER_H

class Logger;

class ArduinoOtaManager {
public:
    static void begin(Logger *logger);
    static void loop();
};

#endif // MODBUS_TO_MQTT_ARDUINOOTAMANAGER_H

