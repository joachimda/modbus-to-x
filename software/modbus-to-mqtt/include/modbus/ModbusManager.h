#ifndef MODBUSMANAGER_H
#define MODBUSMANAGER_H
#include <Preferences.h>
#include <vector>

#include "Logger.h"
#include "config_structs/ModbusDatapoint.h"
#include "config_structs/ConfigurationRoot.h"
#include "modbus/ModbusBus.h"
#include "modbus/ModbusMqttBridge.h"

class MqttManager;

class ModbusManager {
public:
    explicit ModbusManager(Logger *logger);

    bool begin();

    bool loadConfiguration();

    void loop();

    /**
     Execute an adhoc Modbus command against a slave.
     For read functions (1..4), fills outBuf with up to outBufCap words and sets outCount.
     For write functions (5,6,16), writeValue is used if hasWriteValue=true; outCount will be 0 on success.
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
    // Returns true if the new config is loaded and the bus stays active.
    bool reconfigureFromFile();

    static uint16_t sliceRegister(uint16_t word, RegisterSlice slice);

    const ConfigurationRoot &getConfiguration() const;

    static uint32_t getBusErrorCount();

    static void setModbusEnabled(bool enabled);

    static bool getBusState();

private:
    bool readModbusDevice(ModbusDevice &dev, const std::vector<ModbusDatapoint *> &dueDatapoints, uint32_t nowMs);

    static const char *functionToString(ModbusFunctionType fn);

    void incrementBusErrorCount();


    std::vector<ModbusDatapoint> _modbusRegisters;
    ModbusBus _bus;
    ModbusMqttBridge _mqttBridge;
    Logger *_logger;
    Preferences preferences;
    ConfigurationRoot _modbusRoot{};
    MqttManager *_mqtt{nullptr};
    bool _mqttConnectedLastLoop{false};
    std::vector<ModbusDatapoint *> _dueScratch;
};
#endif
