#ifndef MODBUSMANAGER_H
#define MODBUSMANAGER_H
#include <vector>
#include "config_structs/ModbusDatapoint.h"
#include "commlink/CommLink.h"
#include "ModbusMaster.h"
#include "config_structs/ConfigurationRoot.h"

class ModbusManager {
public:
    explicit ModbusManager(Logger *logger);

    bool begin();

    void initializeWiring() const;

    void loop();

private:
    bool readModbusDevice(const ModbusDevice &dev);

    bool loadConfiguration();

    static void preTransmissionHandler();

    static void postTransmissionHandler();

    std::vector<ModbusDatapoint> _modbusRegisters;
    ModbusMaster node;
    Logger *_logger;
    Preferences preferences;

    ConfigurationRoot _modbusRoot{};
};
#endif //MODBUSMANAGER_H
