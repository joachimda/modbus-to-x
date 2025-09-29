#ifndef MODBUSMANAGER_H
#define MODBUSMANAGER_H
#include <Preferences.h>
#include <vector>

#include "Logger.h"
#include "config_structs/ModbusDatapoint.h"
#include "ModbusMaster.h"
#include "config_structs/ConfigurationRoot.h"

class MqttManager;

class ModbusManager {
public:
    explicit ModbusManager(Logger *logger);

    bool begin();

    void initializeWiring() const;

    void loop();

    /**
     Execute an adhoc Modbus command against a slave.
     For read functions (1..4), fills outBuf with up to outBufCap words and sets outCount.
     For write functions (5..6), writeValue is used if hasWriteValue=true; outCount will be 0 on success.
     Returns ModbusMaster status code (0 on success).
    */
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

    uint8_t findSlaveIdByDatapointId(const String &dpId) const;
    const ModbusDatapoint *findDatapointById(const String &dpId, const ModbusDevice **outDevice = nullptr) const;
    void setMqttManager(MqttManager *mqtt);

    static const char *statusToString(uint8_t code);
    static String registersToAscii(const uint16_t *buf, uint16_t count);

    // Reload /conf/config.json at runtime and reinitialize wiring.
    // Returns true if new config loaded and bus stays active.
    bool reconfigureFromFile();

private:
    bool readModbusDevice(const ModbusDevice &dev);

    bool loadConfiguration();

    static void preTransmissionHandler();

    static void postTransmissionHandler();

    static const char *functionToString(ModbusFunctionType fn);

    void publishDatapoint(const ModbusDevice &device, const ModbusDatapoint &dp, const String &payload) const;
    static String slugify(const String &text);

    std::vector<ModbusDatapoint> _modbusRegisters;
    ModbusMaster node;
    Logger *_logger;
    Preferences preferences;
    ConfigurationRoot _modbusRoot{};
    MqttManager *_mqtt{nullptr};
};
#endif
