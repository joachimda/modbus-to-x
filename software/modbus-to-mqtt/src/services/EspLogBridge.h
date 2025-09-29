#pragma once

#include <stdarg.h>
class MemoryLogger;

// Bridges ESP-IDF ESP_LOGx output into MemoryLogger (and still prints to serial)
namespace EspLogBridge {
    void begin(MemoryLogger *mem);
}

