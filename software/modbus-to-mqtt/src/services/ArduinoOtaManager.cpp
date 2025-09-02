#include "services/ArduinoOtaManager.h"
#if DEV_OTA_ENABLED
#include <ArduinoOTA.h>
#include "services/IndicatorService.h"
#include "Logger.h"

static Logger *g_logger = nullptr;

void ArduinoOtaManager::begin(Logger *logger) {
    g_logger = logger;

    ArduinoOTA
        .onStart([] {
            IndicatorService::instance().setOtaActive(true);
            if (g_logger) g_logger->logInformation("ArduinoOTA: Start");
        })
        .onEnd([] {
            IndicatorService::instance().setOtaActive(false);
            if (g_logger) g_logger->logInformation("ArduinoOTA: End");
        })
        .onProgress([](const unsigned int progress, const unsigned int total) {
            (void)progress; (void)total;
        })
        .onError([](const ota_error_t error) {
            if (g_logger) g_logger->logError((String("ArduinoOTA Error: ") + String(error)).c_str());
        });

    ArduinoOTA.setPassword(DEV_OTA_ARDUINO_PASS);
    ArduinoOTA.begin();
    if (g_logger) g_logger->logInformation("ArduinoOTA: Ready");
}

void ArduinoOtaManager::loop() {
    ArduinoOTA.handle();
}
#else
void ArduinoOtaManager::begin(Logger *) {}
void ArduinoOtaManager::loop() {}
#endif
