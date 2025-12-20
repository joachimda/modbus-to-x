#ifndef MODBUS_TOPIC_BUILDER_H
#define MODBUS_TOPIC_BUILDER_H

#include "modbus/config_structs/ModbusDevice.h"
#include "modbus/config_structs/ModbusDatapoint.h"

class ModbusTopicBuilder {
public:
    explicit ModbusTopicBuilder(String rootTopic);

    String datapointTopic(const ModbusDevice &device, const ModbusDatapoint &dp) const;

    String availabilityTopic(const ModbusDevice &device) const;

    static String deviceSegment(const ModbusDevice &device);

    static String datapointSegment(const ModbusDatapoint &dp);

    static String friendlyName(const ModbusDevice &device, const ModbusDatapoint &dp);

private:
    String _rootTopic;
};

#endif
