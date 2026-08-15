#ifndef MODBUSCONFIGLOADER_H
#define MODBUSCONFIGLOADER_H

#include "Logger.h"
#include "config_structs/ConfigurationRoot.h"

class ModbusConfigLoader {
public:
    // Loads configuration from the given config filesystem path into outConfig.
    // Returns true on successful load and parse, false if file missing or parse error.
    static bool loadConfiguration(Logger *logger, const char *path, ConfigurationRoot &outConfig);

    // Parses an owned request body without consulting the shared config file.
    static bool parseConfiguration(Logger *logger, const char *json, size_t length,
                                   ConfigurationRoot &outConfig);
};

#endif // MODBUSCONFIGLOADER_H
