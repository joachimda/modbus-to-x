#include "modbus/ModbusTopicBuilder.h"

#include "utils/StringUtils.h"

ModbusTopicBuilder::ModbusTopicBuilder(String rootTopic) : _rootTopic(std::move(rootTopic)) {
    _rootTopic.trim();
}

String ModbusTopicBuilder::datapointTopic(const ModbusDevice &device, const ModbusDatapoint &dp) const {
    String topic = dp.topic;
    topic.trim();
    if (topic.length()) {
        return topic;
    }

    const String deviceSeg = deviceSegment(device);
    const String dpSeg = datapointSegment(dp);

    String resolved;
    resolved.reserve(_rootTopic.length() + deviceSeg.length() + dpSeg.length() + 2);
    if (_rootTopic.length()) {
        resolved = _rootTopic;
        if (!resolved.endsWith("/")) {
            resolved += "/";
        }
        resolved += deviceSeg;
    } else {
        resolved = deviceSeg;
    }
    resolved += "/";
    resolved += dpSeg;
    return resolved;
}

String ModbusTopicBuilder::availabilityTopic(const ModbusDevice &device) const {
    const String deviceSeg = deviceSegment(device);

    String topic;
    if (_rootTopic.length()) {
        topic = _rootTopic;
        if (!topic.endsWith("/")) {
            topic += "/";
        }
        topic += deviceSeg;
    } else {
        topic = deviceSeg;
    }
    topic += "/status";
    return topic;
}

String ModbusTopicBuilder::deviceSegment(const ModbusDevice &device) {
    String deviceName = device.name;
    deviceName.trim();
    String segment = StringUtils::slugify(deviceName);
    if (!segment.length()) {
        String fallbackId = device.id;
        fallbackId.trim();
        if (!fallbackId.length()) {
            fallbackId = String("device_") + String(device.slaveId);
        }
        segment = StringUtils::slugify(fallbackId);
    }
    if (!segment.length()) {
        segment = String("device");
    }
    return segment;
}

String ModbusTopicBuilder::datapointSegment(const ModbusDatapoint &dp) {
    String dpName = dp.name;
    dpName.trim();
    String segment = StringUtils::slugify(dpName);
    if (!segment.length()) {
        const int separatorIndex = dp.id.lastIndexOf('.');
        if (separatorIndex >= 0) {
            const auto nextIndex = static_cast<unsigned int>(separatorIndex + 1);
            if (nextIndex < dp.id.length()) {
                segment = StringUtils::slugify(dp.id.substring(nextIndex));
            } else {
                segment = StringUtils::slugify(dp.id);
            }
        } else {
            segment = StringUtils::slugify(dp.id);
        }
    }
    if (!segment.length()) {
        segment = String("datapoint");
    }
    return segment;
}

String ModbusTopicBuilder::friendlyName(const ModbusDevice &device, const ModbusDatapoint &dp) {
    auto toTitle = [](String value) {
        value.trim();
        if (!value.length()) {
            return value;
        }
        String result;
        result.reserve(value.length() + 4);
        bool newWord = true;
        for (size_t i = 0; i < value.length(); ++i) {
            const auto raw = static_cast<unsigned char>(value[i]);
            if (raw == '_' || raw == '-' || raw == '.') {
                if (result.length() && result[result.length() - 1] != ' ') {
                    result += ' ';
                }
                newWord = true;
                continue;
            }
            if (newWord) {
                result += static_cast<char>(std::toupper(raw));
                newWord = false;
            } else {
                result += static_cast<char>(std::tolower(raw));
            }
        }
        while (result.length() && result[result.length() - 1] == ' ') {
            result.remove(result.length() - 1);
        }
        return result;
    };

    String deviceLabel = toTitle(device.name);
    if (!deviceLabel.length()) {
        deviceLabel = toTitle(device.id);
    }
    if (!deviceLabel.length()) {
        deviceLabel = toTitle(deviceSegment(device));
    }

    String datapointLabel = toTitle(dp.name);
    if (!datapointLabel.length()) {
        datapointLabel = toTitle(dp.id);
    }

    if (deviceLabel.length() && datapointLabel.length()) {
        return deviceLabel + " " + datapointLabel;
    }
    if (deviceLabel.length()) {
        return deviceLabel;
    }
    return datapointLabel;
}

