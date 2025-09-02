#ifndef LED_SERVICE_H
#define LED_SERVICE_H

#include <atomic>
#include <Arduino.h>
class IndicatorService {
public:
    static IndicatorService &instance();

    void begin();

    void setPortalMode(bool on);

    void setWifiConnected(bool on);

    void setMqttConnected(bool on);

    void setModbusConnected(bool on);

    void setOtaActive(bool on);

    IndicatorService(const IndicatorService &) = delete;

    IndicatorService &operator=(const IndicatorService &) = delete;

    IndicatorService(IndicatorService &&) = delete;

    IndicatorService &operator=(IndicatorService &&) = delete;

private:
    IndicatorService() = default;

    [[noreturn]] static void taskRunner(void *param);

    std::atomic<bool> _portal{false};
    std::atomic<bool> _ota{false};
    std::atomic<bool> _wifi{false};
    std::atomic<bool> _mqtt{false};
    std::atomic<bool> _modbus{false};
};

#endif // LED_SERVICE_H
