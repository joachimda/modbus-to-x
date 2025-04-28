#ifndef MODBUSMANAGER_H
#define MODBUSMANAGER_H
#include <ModbusMaster.h>
#include <vector>
#include "ModbusRegister.h"
#include "commlink/CommLink.h"

class ModbusManager {
public:
    explicit ModbusManager(CommLink *commLink, Logger *logger);

    void initialize();

    void readSwVersion();

    void readRegisters();

private:
    static void preTransmissionHandler();

    static void postTransmissionHandler();

    static std::vector<ModbusRegister> setupInputRegisters();

    std::vector<ModbusRegister> sensorRegisters;
    ModbusMaster node;
    CommLink *_commLink;
    Logger * _logger;
};
#endif //MODBUSMANAGER_H
