#ifndef MODBUSUSERCONFIG_H
#define MODBUSUSERCONFIG_H
#include <map>
#include <WString.h>

struct ModbusUserConfig {
    uint32_t communicationMode;
    int baudRate;
};

static const std::map<String, uint32_t> communicationModes = {
    {"8N1", SERIAL_8N1},
    {"8N2", SERIAL_8N2},
    {"8E1", SERIAL_8E1},
    {"8E2", SERIAL_8E2},
    {"8O1", SERIAL_8O1},
    {"8O2", SERIAL_8O2}
};

static const std::map<uint32_t, String> communicationModesBackwards = {
    {SERIAL_8N1, "8N1"},
    {SERIAL_8N2, "8N2"},
    {SERIAL_8E1, "8E1"},
    {SERIAL_8E2, "8E2"},
    {SERIAL_8O1,"8O1"},
    {SERIAL_8O2,"8O2"}
};

#endif //MODBUSUSERCONFIG_H
