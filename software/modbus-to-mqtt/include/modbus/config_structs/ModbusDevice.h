#ifndef MODBUS_TO_MQTT_MODBUSDEVICE_H
#define MODBUS_TO_MQTT_MODBUSDEVICE_H
#include <cstddef>
#include <vector>
#include <WString.h>

#include "concurrency/PublicationAttemptCore.h"
#include "ModbusDatapoint.h"

struct ModbusDevice {
    String id;
    String name;
    uint8_t slaveId;
    bool mqttEnabled{false};
    bool homeassistantDiscoveryEnabled{false};
    bool haAvailabilityOnlinePublished{false};
    bool haDiscoveryPublished{false};
    bool haAvailabilityPublishPending{false};
    PublicationAttemptCore haAvailabilityPublishAttempt;
    uint16_t haDiscoveryPublishPending{0U};
    PublicationAttemptCore haDiscoveryPublishAttempt;
    bool haDiscoveryPublishFailed{false};
    size_t haDiscoveryPublishCursor{0U};
    bool haDiscoveryPublishSchedulingComplete{false};
    std::vector<ModbusDatapoint> datapoints;
};

#endif
