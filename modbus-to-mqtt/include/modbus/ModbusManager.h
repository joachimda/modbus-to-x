#ifndef MODBUSMANAGER_H
#define MODBUSMANAGER_H
#include <ModbusMaster.h>
#include <vector>
#include "ModbusRegister.h"
#include "commlink/CommLink.h"

class ModbusManager {
public:
    explicit ModbusManager(Logger *logger);

    void initialize();

    void readRegisters();

    void clearRegisters();

    String getRegisterConfigurationAsJson() const;

    void loadRegisterConfig();

    static uint16_t getRegisterCount();

    void updateRegisterConfigurationFromJson(const String& registerConfigJson, bool clearExisting);

    std::vector<ModbusRegister> getRegisters() const;
private:
    static void preTransmissionHandler();

    void addRegister(const ModbusRegister& reg);

    static void postTransmissionHandler();

    void saveRegisters() const;

    std::vector<ModbusRegister> _modbusRegisters;
    ModbusMaster node;
    Logger * _logger;
};
#endif //MODBUSMANAGER_H
