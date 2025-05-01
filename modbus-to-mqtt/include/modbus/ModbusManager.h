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
    void addRegister(const ModbusRegister& reg);
    void updateRegisterConfigurationFromJson(const String& registerConfigJson);

private:
    static void preTransmissionHandler();

    static void postTransmissionHandler();

    void saveRegisters() const;

    static std::vector<ModbusRegister> setupInputRegisters();

    std::vector<ModbusRegister> sensorRegisters;
    ModbusMaster node;
    Logger * _logger;
};
#endif //MODBUSMANAGER_H
