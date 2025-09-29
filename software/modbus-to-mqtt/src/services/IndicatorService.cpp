#include "services/IndicatorService.h"
#include "Config.h"

static constexpr uint32_t BLINK_INTERVAL_MS = 300;
static constexpr uint32_t BLINK_FAST_INTERVAL_MS = 100;

IndicatorService &IndicatorService::instance() {
    static IndicatorService s;
    return s;
}

void IndicatorService::begin() {
    pinMode(LED_A_PIN, OUTPUT);
    pinMode(LED_B_PIN, OUTPUT);
    pinMode(LED_C_PIN, OUTPUT);

    xTaskCreatePinnedToCore(
        taskRunner,
        "IndicatorService",
        2048,
        this,
        1,
        nullptr,
        1
    );
}

void IndicatorService::setPortalMode(const bool on) { _portal.store(on, std::memory_order_release); }
void IndicatorService::setWifiConnected(const bool on) { _wifi.store(on, std::memory_order_release); }
void IndicatorService::setMqttConnected(const bool on) { _mqtt.store(on, std::memory_order_release); }
void IndicatorService::setModbusConnected(const bool on) { _modbus.store(on, std::memory_order_release); }
void IndicatorService::setOtaActive(const bool on) { _ota.store(on, std::memory_order_release); }

void IndicatorService::taskRunner(void *param) {
    auto *self = static_cast<IndicatorService *>(param);
    bool phase = false;
    TickType_t delayTicks = BLINK_INTERVAL_MS / portTICK_PERIOD_MS;

    for (;;) {
        const bool ota = self->_ota.load(std::memory_order_acquire);
        const bool portal = self->_portal.load(std::memory_order_acquire);
        if (ota) {
            phase = !phase;
            digitalWrite(LED_A_PIN, phase ? HIGH : LOW);
            digitalWrite(LED_B_PIN, phase ? HIGH : LOW);
            digitalWrite(LED_C_PIN, phase ? HIGH : LOW);
            delayTicks = BLINK_FAST_INTERVAL_MS / portTICK_PERIOD_MS;
        } else if (portal) {
            phase = !phase;
            digitalWrite(LED_A_PIN, phase ? HIGH : LOW);
            digitalWrite(LED_B_PIN, phase ? HIGH : LOW);
            digitalWrite(LED_C_PIN, phase ? HIGH : LOW);
            delayTicks = BLINK_INTERVAL_MS / portTICK_PERIOD_MS;
        } else {
            digitalWrite(LED_A_PIN, self->_wifi.load(std::memory_order_acquire) ? HIGH : LOW);
            digitalWrite(LED_B_PIN, self->_mqtt.load(std::memory_order_acquire) ? HIGH : LOW);
            digitalWrite(LED_C_PIN, self->_modbus.load(std::memory_order_acquire) ? HIGH : LOW);
            delayTicks = BLINK_INTERVAL_MS / portTICK_PERIOD_MS;
        }
        vTaskDelay(delayTicks);
    }
}
