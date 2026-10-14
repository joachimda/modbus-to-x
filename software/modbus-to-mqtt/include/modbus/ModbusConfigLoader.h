#ifndef MODBUSCONFIGLOADER_H
#define MODBUSCONFIGLOADER_H

#include "Logger.h"
#include "modbus/ModbusConfigDocument.h"
#include "config_structs/ConfigurationRoot.h"

class ModbusConfigLoader {
public:
    // Loads configuration from the given config filesystem path into outConfig.
    // Returns true on successful load and parse, false if file missing or parse error.
    // Parses leniently: a stored document keeps loading with logged adjustments.
    static bool loadConfiguration(Logger *logger, const char *path, ConfigurationRoot &outConfig);

    // Parses an owned request body without consulting the shared config file.
    // Strict by default: an API submission with an unrepairable datapoint is rejected.
    static bool parseConfiguration(Logger *logger, const char *json, size_t length,
                                   ConfigurationRoot &outConfig,
                                   ModbusConfigDocument::ValidationError *validationError = nullptr,
                                   ModbusConfigDocument::ParseMode mode =
                                       ModbusConfigDocument::ParseMode::Strict);
};

#endif // MODBUSCONFIGLOADER_H
