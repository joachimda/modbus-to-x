#ifndef MODBUSMANAGER_H
#define MODBUSMANAGER_H
#include <Preferences.h>
#include <vector>

#include "Logger.h"
#include "config_structs/ModbusDatapoint.h"
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

    // Diagnostics helpers
    static const char *statusToString(uint8_t code);
    static const char *functionToString(ModbusFunctionType fn);
    void scanSlaveIds(uint8_t startId, uint8_t endId,
                      ModbusFunctionType fn, uint16_t address, uint8_t numRegs);

    std::vector<ModbusDatapoint> _modbusRegisters;
    ModbusMaster node;
    Logger *_logger;
    Preferences preferences;

    ConfigurationRoot _modbusRoot{};
    bool _scanAttempted{false};
};
#endif
