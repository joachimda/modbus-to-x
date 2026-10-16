#ifndef MODBUSBUS_H
#define MODBUSBUS_H

#include <atomic>
#include "ModbusMaster.h"
#include "modbus/Rs485Port.h"
#include "modbus/config_structs/Bus.h"

class Logger;

// The RS-485 port together with the ModbusMaster node that speaks standard
// Modbus RTU on it, and the enable flag that the configuration and the OTA
// quiescence toggle. Protocols ModbusMaster does not speak (Wavin) use
// port() directly.
class ModbusBus {
public:
    explicit ModbusBus(Logger *logger);

    bool begin(const Bus &busConfig);

    Rs485Port &port();

    // Bound to the port's direction control. Call
    // node().begin(slave, port().stream()) before a transaction.
    ModbusMaster &node();

    bool isActive() const;

    void setActive(bool enabled);

private:
    static void preTransmitTrampoline();

    static void postTransmitTrampoline();

    Rs485Port _port;
    ModbusMaster _node;
    std::atomic<bool> _active{false};

    static ModbusBus *s_instance;
};

#endif
