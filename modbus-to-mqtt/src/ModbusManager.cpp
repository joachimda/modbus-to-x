#include "Config.h"
#include "Modbus/ModbusManager.h"
#include "modbus/ModbusRegister.h"

ModbusManager::ModbusManager(CommLink *commLink, Logger *logger) : _commLink(commLink), _logger(logger) {}
void ModbusManager::initialize() {

    _commLink->mqttPublish("log", "MBManager::ModbusManager");

    pinMode(RS485_DE_PIN, OUTPUT);
    pinMode(RS485_RE_PIN, OUTPUT);

    digitalWrite(RS485_DE_PIN, LOW);  // Default to receive mode
    digitalWrite(RS485_RE_PIN, LOW);  // Default to receive mode

    Serial1.begin(MODBUS_BAUD_RATE, SERIAL_8N1, RX, TX);
    node.begin(MODBUS_SLAVE_ID, Serial1);

    node.preTransmission(preTransmissionHandler);
    node.postTransmission(postTransmissionHandler);
    sensorRegisters = setupInputRegisters();
    _logger->logInformation("ModbusManager::initialize - Finished");

}
void ModbusManager::readRegisters() {
    for (auto& reg : sensorRegisters) {
        _logger->logInformation(("Reading register: " + String(reg.name)).c_str());

        const uint8_t result = reg.registerType == INPUT_REGISTER
                             ? node.readInputRegisters(reg.address, reg.numOfRegisters)
                             : node.readHoldingRegisters(reg.address, reg.numOfRegisters);

        if (result == ModbusMaster::ku8MBSuccess) {
            String topic = reg.name;
            const float rawData = node.getResponseBuffer(0);
            const float value = rawData * reg.scale;
            auto payload = String(value);
            _logger->logInformation(("After Read register: " + String(reg.name) + " Data: " + String(payload)).c_str());

        } else {
            _logger->logInformation(("Error reading register: " + String(reg.name) + " Error code: " + String(result)).c_str());
        }
    }
}

void ModbusManager::preTransmissionHandler() {
    digitalWrite(RS485_DE_PIN, HIGH);
    digitalWrite(RS485_RE_PIN, HIGH);
}

void ModbusManager::postTransmissionHandler() {
    digitalWrite(RS485_DE_PIN, LOW);
    digitalWrite(RS485_RE_PIN, LOW);
}

std::vector<ModbusRegister> ModbusManager::setupInputRegisters() {
    std::vector<ModbusRegister> inputRegisters;
    // inputRegisters.emplace_back("log", 0, 1.0, MBRegisterType::INPUT_REGISTER, MBDataType::NUMBER);
    // inputRegisters.emplace_back("ventilation/system/mode", 1001, 1.0, MBRegisterType::INPUT_REGISTER);
    // inputRegisters.emplace_back("ventilation/system/state", 1002, 1.0, MBRegisterType::INPUT_REGISTER);
    return inputRegisters;
}