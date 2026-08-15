#ifndef MODBUSMANAGER_H
#define MODBUSMANAGER_H

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <vector>

#include "Config.h"
#include "Logger.h"
#include "concurrency/CoherentSnapshotCore.h"
#include "concurrency/LatestRevisionCore.h"
#include "config_structs/ConfigurationRoot.h"
#include "modbus/ModbusBus.h"
#include "modbus/ModbusMqttBridge.h"
#include "modbus/ModbusRuntimeTypes.h"

class MqttManager;

// Ownership contract:
// - the Arduino loop task is the sole owner of _modbusRoot, polling scratch
//   pointers, ModbusBus initialization and every bus transaction;
// - HTTP, MQTT and other tasks submit owned requests through the bounded queue;
// - readers consume narrow value snapshots and never retain live pointers.
class ModbusManager {
public:
    explicit ModbusManager(Logger *logger);

    bool begin();

    void loop();

    ModbusCommandResult executeCommand(const ModbusCommandRequest &request);

    ModbusOperationResult submitWriteCommand(const ModbusCommandRequest &request);

    bool reconfigureFromFile();

    ModbusOperationResult requestReconfigureFromFile();

    ModbusOperationResult requestReconfigure(const String &configurationJson,
                                             uint32_t revision);

    uint32_t issueConfigurationRevision();

    bool isConfigurationRevisionObsolete(uint32_t revision) const;

    ModbusOperationResult setEnabled(bool enabled);

    ModbusRuntimeSnapshot getRuntimeSnapshot() const;

    ModbusOperationResult submitPublicationCompletion(
        const ModbusPublicationCompletion &completion);

    void setMqttManager(MqttManager *mqtt);

    static const char *statusToString(uint8_t code);

    static String registersToAscii(const uint16_t *buffer, uint16_t count);

    static uint16_t sliceRegister(uint16_t word, RegisterSlice slice);

private:
    struct Command;

    bool loadConfigurationCandidate(ConfigurationRoot &candidate) const;

    ModbusOperationResult activateConfiguration(ConfigurationRoot candidate,
                                                uint32_t revision = 0U);

    ModbusOperationResult submitAndWait(Command *command, uint32_t waitMs = MODBUS_COMMAND_WAIT_MS,
                                        ModbusCommandResult *result = nullptr,
                                        LatestRevisionCore::Admission *admission = nullptr);

    ModbusOperationResult submitAsync(Command *command);

    void drainCommands();

    void processCommand(Command &command);

    ModbusCommandResult executeOwned(const ModbusCommandRequest &request);

    bool readModbusDevice(ModbusDevice &device, const std::vector<ModbusDatapoint *> &dueDatapoints,
                          uint32_t nowMs);

    static const char *functionToString(ModbusFunctionType function);

    void incrementBusErrorCount();

    void updateRuntimeSnapshot();

    bool inOwnerContext() const;

    void recordOwnerViolation();

    ModbusBus _bus;
    ModbusMqttBridge _mqttBridge;
    Logger *_logger;
    ConfigurationRoot _modbusRoot{};
    MqttManager *_mqtt{nullptr};
    uint32_t _generation{0U};
    LatestRevisionCore _configurationRevisions;
    std::vector<ModbusDatapoint *> _dueScratch;
    QueueHandle_t _commandQueue{nullptr};
    TaskHandle_t _ownerTaskHandle{nullptr};
    std::atomic<bool> _ownerAvailable{false};
    std::atomic<bool> _shuttingDown{false};
    CoherentSnapshotCore<ModbusRuntimeSnapshot> _runtimeSnapshot;
    std::atomic<uint32_t> _ownerViolationCount{0U};
    std::atomic<uint32_t> _commandFailureCount{0U};
};

#endif
