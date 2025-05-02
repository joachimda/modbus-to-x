#include "Config.h"
#include "Modbus/ModbusManager.h"
#include "ArduinoJson.h"

ModbusManager::ModbusManager(Logger *logger) : _logger(logger) {
}

void ModbusManager::initialize() {
    _logger->logDebug("ModbusManager::initialize - Entry");

    pinMode(RS485_DE_PIN, OUTPUT);
    pinMode(RS485_RE_PIN, OUTPUT);

    digitalWrite(RS485_DE_PIN, LOW);
    digitalWrite(RS485_RE_PIN, LOW);

    Serial1.begin(MODBUS_BAUD_RATE, SERIAL_8N1, RX, TX);
    node.begin(MODBUS_SLAVE_ID, Serial1);

    node.preTransmission(preTransmissionHandler);
    node.postTransmission(postTransmissionHandler);

    loadRegisterConfig();

    _logger->logDebug("ModbusManager::initialize - Exit");
}

void ModbusManager::readRegisters() {
    for (auto &reg: _modbusRegisters) {
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

void ModbusManager::clearRegisters() {
    _modbusRegisters.clear();
    Preferences preferences;
    preferences.begin(MODBUS_PREFS_NAMESPACE, false);
    preferences.putUShort(REG_COUNT_KEY, 0);
    preferences.end();
}

String ModbusManager::getRegisterConfigurationAsJson() const {
    JsonDocument doc;
    const auto json_array = doc.to<JsonArray>();

    for (const auto& reg : _modbusRegisters) {
        auto obj = json_array.add<JsonObject>();
        obj["address"] = reg.address;
        obj["numOfRegisters"] = reg.numOfRegisters;
        obj["scale"] = reg.scale;
        obj["registerType"] = reg.registerType;
        obj["dataType"] = reg.dataType;
        obj["name"] = reg.name;
    }

    String out;
    serializeJson(doc, out);
    return out;
}

void ModbusManager::updateRegisterConfigurationFromJson(const String &registerConfigJson, bool clearExisting) {
    JsonDocument doc;
    _logger->logInformation((registerConfigJson).c_str());
    const DeserializationError err =  deserializeJson(doc, registerConfigJson);

    if (err) {
        _logger->logError(("ModbusManager::updateRegistersFromJson - Error parsing register config: " + String(err.c_str())).c_str());
        return;
    }

    const auto arr = doc.as<JsonArray>();
    if(clearExisting)
    {
        clearRegisters();
    }

    for (JsonObject obj : arr) {
        ModbusRegister reg{};

        reg.address = obj["address"] | 0;
        reg.numOfRegisters = obj["numOfRegisters"] | 1;
        reg.scale = obj["scale"] | 1.0f;
        reg.registerType = static_cast<RegisterType>(obj["registerType"] | 1);
        reg.dataType = static_cast<ModbusDataType>(obj["dataType"] | 2);
        const char* name = obj["name"] | "unnamed";
        strncpy(reg.name, name, sizeof(reg.name));
        reg.name[sizeof(reg.name) - 1] = '\0';

        _modbusRegisters.push_back(reg);
    }
    _logger->logInformation(("Loaded " + String(_modbusRegisters.size()) + " registers from JSON").c_str());
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

void ModbusManager::loadRegisterConfig() {
    Preferences preferences;
    preferences.begin(MODBUS_PREFS_NAMESPACE, true);
    const uint16_t count = preferences.getUShort(REG_COUNT_KEY, 0);
    _logger->logDebug(("ModbusManager::loadRegisterConfig - Found " + String(count) + " registers").c_str());
    if (count > 0 && count < MODBUS_MAX_REGISTERS + 1) {
        _modbusRegisters.resize(count);
        preferences.getBytes(REG_DATA_KEY, _modbusRegisters.data(), count * sizeof(ModbusRegister));
    }
    preferences.end();
}

void ModbusManager::addRegister(const ModbusRegister& reg) {
    _modbusRegisters.push_back(reg);
    saveRegisters();
}

void ModbusManager::saveRegisters() const {
    Preferences preferences;
    preferences.begin(MODBUS_PREFS_NAMESPACE, false);
    const uint16_t count = _modbusRegisters.size();
    preferences.putUShort(REG_COUNT_KEY, count);
    preferences.putBytes(REG_DATA_KEY, _modbusRegisters.data(), count * sizeof(ModbusRegister));
    preferences.end();
}

