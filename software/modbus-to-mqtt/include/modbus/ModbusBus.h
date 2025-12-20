#ifndef MODBUSBUS_H
#define MODBUSBUS_H

#include <atomic>
#include <Arduino.h>
#include "ModbusMaster.h"
#include "modbus/config_structs/Bus.h"
#include "utils/TeeStream.h"

class Logger;

class ModbusBus {
public:
    class Guard {
    public:
        Guard(ModbusBus &bus, bool owns);
        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;
        Guard(Guard &&other) noexcept;
        Guard &operator=(Guard &&other) noexcept;
        ~Guard();
        explicit operator bool() const { return _owns; }
    private:
        ModbusBus *_bus;
        bool _owns;
    };

    explicit ModbusBus(Logger *logger);

    bool begin(const Bus &busConfig);

    Guard acquire();

    ModbusMaster &node();

    Stream &stream();

    bool isActive() const;

    void setActive(bool enabled);

    bool isInitialized() const;

    void incrementError();

    uint32_t errorCount() const;

    String dumpRx() const;

    static uint32_t getErrorCount();

    static bool isEnabled();

    static void setEnabled(bool enabled);

    bool isBusy() const;

    void enableCapture(bool enable);

private:
    void initializeWiring(const Bus &busConfig);

    void onPreTransmission();

    void onPostTransmission();

    static void preTransmitTrampoline();

    static void postTransmitTrampoline();

    void release();

    Logger *_logger;
    ModbusMaster _node;
    TeeStream *_tee{nullptr};
    std::atomic<bool> _active{false};
    std::atomic<bool> _initialized{false};
    std::atomic<bool> _busy{false};
    std::atomic<uint32_t> _errorCount{0};

    static ModbusBus *s_instance;
};

#endif
