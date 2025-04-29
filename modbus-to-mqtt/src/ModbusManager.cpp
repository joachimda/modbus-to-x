#include "Config.h"
#include "Modbus/ModbusManager.h"
#include "modbus/ModbusRegister.h"
#include "ArduinoJson.h"

ModbusManager::ModbusManager(Logger *logger) : _logger(logger) {
}

void ModbusManager::initialize() {
    _logger->logDebug("ModbusManager::initialize");

    pinMode(RS485_DE_PIN, OUTPUT);
    pinMode(RS485_RE_PIN, OUTPUT);

    digitalWrite(RS485_DE_PIN, LOW);
    digitalWrite(RS485_RE_PIN, LOW);

    Serial1.begin(MODBUS_BAUD_RATE, SERIAL_8N1, RX, TX);
    node.begin(MODBUS_SLAVE_ID, Serial1);

    node.preTransmission(preTransmissionHandler);
    node.postTransmission(postTransmissionHandler);

    loadRegisters();

    sensorRegisters = setupInputRegisters();
    _logger->logInformation("ModbusManager::initialize - Finished");
}

void ModbusManager::readRegisters() {
    for (auto &reg: sensorRegisters) {
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
            _logger->logInformation(
                ("Error reading register: " + String(reg.name) + " Error code: " + String(result)).c_str());
        }
    }
}

void ModbusManager::updateRegistersFromJson(const String &registerConfigJson) {
    JsonDocument doc;
    _logger->logInformation((registerConfigJson).c_str());
    DeserializationError err =  deserializeJson(doc, registerConfigJson);

    if (err) {
        _logger->logError(("Error parsing register config: " + String(err.c_str())).c_str());
        return;
    }

    const auto arr = doc.as<JsonArray>();
    sensorRegisters.clear();

    for (JsonObject obj : arr) {
        ModbusRegister reg{};

        reg.address = obj["address"] | 0;
        reg.numOfRegisters = obj["numOfRegisters"] | 1;
        reg.scale = obj["scale"] | 1.0;
        reg.registerType = static_cast<RegisterType>(obj["registerType"] | 1);
        reg.dataType = static_cast<ModbusDataType>(obj["dataType"] | 2);
        const char* name = obj["name"] | "unnamed";
        strncpy(reg.name, name, sizeof(reg.name));
        reg.name[sizeof(reg.name) - 1] = '\0';

        sensorRegisters.push_back(reg);
    }
    _logger->logInformation(("Loaded " + String(sensorRegisters.size()) + " registers from JSON").c_str());
    saveRegisters();
}

void ModbusManager::preTransmissionHandler() {
    digitalWrite(RS485_DE_PIN, HIGH);
    digitalWrite(RS485_RE_PIN, HIGH);
}

void ModbusManager::postTransmissionHandler() {
    digitalWrite(RS485_DE_PIN, LOW);
    digitalWrite(RS485_RE_PIN, LOW);
}

void ModbusManager::loadRegisters() {
    Preferences prefs;
    prefs.begin(MODBUS_PREFS_NAMESPACE, true);
    const uint16_t count = prefs.getUShort(REG_COUNT_KEY, 0);
    if (count > 0 && count < MODBUS_MAX_REGISTERS + 1) {
        sensorRegisters.resize(count);
        prefs.getBytes(REG_DATA_KEY, sensorRegisters.data(), count * sizeof(ModbusRegister));
    }
    prefs.end();
}

void ModbusManager::addRegister(const ModbusRegister& reg) {
    sensorRegisters.push_back(reg);
    saveRegisters();
}

void ModbusManager::saveRegisters() const {
    Preferences prefs;
    prefs.begin(MODBUS_PREFS_NAMESPACE, false);
    const uint16_t count = sensorRegisters.size();
    prefs.putUShort(REG_COUNT_KEY, count);
    prefs.putBytes(REG_DATA_KEY, sensorRegisters.data(), count * sizeof(ModbusRegister));
    prefs.end();
}


std::vector<ModbusRegister> ModbusManager::setupInputRegisters() {
    std::vector<ModbusRegister> inputRegisters;
    // inputRegisters.emplace_back("log", 0, 1.0, MBRegisterType::INPUT_REGISTER, MBDataType::NUMBER);
    // inputRegisters.emplace_back("ventilation/system/mode", 1001, 1.0, MBRegisterType::INPUT_REGISTER);
    // inputRegisters.emplace_back("ventilation/system/state", 1002, 1.0, MBRegisterType::INPUT_REGISTER);
    return inputRegisters;
}

