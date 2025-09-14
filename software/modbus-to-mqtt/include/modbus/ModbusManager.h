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

    // Execute an ad-hoc Modbus command against a slave.
    // - For read functions (1..4), fills outBuf with up to outBufCap words and sets outCount.
    // - For write functions (5..6), uses writeValue if hasWriteValue=true; outCount will be 0 on success.
    // Returns ModbusMaster status code (0 on success).
    uint8_t executeCommand(uint8_t slaveId,
                           int function,
                           uint16_t addr,
                           uint16_t len,
                           uint16_t writeValue,
                           bool hasWriteValue,
                           uint16_t *outBuf,
                           uint16_t outBufCap,
                           uint16_t &outCount,
                           String &rxDump);

    // Find the slaveId for a datapoint id (unique across devices). Returns 0 if not found.
    uint8_t findSlaveIdByDatapointId(const String &dpId) const;

private:
    bool readModbusDevice(const ModbusDevice &dev);

    bool loadConfiguration();

    static void preTransmissionHandler();

    static void postTransmissionHandler();

    // Diagnostics helpers
    static const char *statusToString(uint8_t code);
    static const char *functionToString(ModbusFunctionType fn);
    std::vector<ModbusDatapoint> _modbusRegisters;
    ModbusMaster node;
    Logger *_logger;
    Preferences preferences;
    ConfigurationRoot _modbusRoot{};
};
#endif
