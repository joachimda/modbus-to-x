#ifndef MODBUS_TOPIC_BUILDER_H
#define MODBUS_TOPIC_BUILDER_H

#include "modbus/config_structs/ModbusDevice.h"
#include "modbus/config_structs/ModbusDatapoint.h"

class ModbusTopicBuilder {
public:
    explicit ModbusTopicBuilder(String rootTopic);

    String datapointTopic(const ModbusDevice &device, const ModbusDatapoint &dp) const;

    // Command topic of a writable datapoint: its resolved base topic (explicit
    // override or default pattern) with "/set" appended. State is published on
    // the base, commands are received only on <base>/set.
    String commandTopic(const ModbusDevice &device, const ModbusDatapoint &dp) const;

    static String commandTopicForBase(String baseTopic);

    String availabilityTopic(const ModbusDevice &device) const;

    static String deviceSegment(const ModbusDevice &device);

    static String datapointSegment(const ModbusDatapoint &dp);

    static String friendlyName(const ModbusDevice &device, const ModbusDatapoint &dp);

private:
    String _rootTopic;
};

#endif
