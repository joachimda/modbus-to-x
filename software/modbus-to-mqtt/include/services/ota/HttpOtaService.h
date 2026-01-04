#ifndef MODBUS_TO_MQTT_HTTPOTAMANAGER_H
#define MODBUS_TO_MQTT_HTTPOTAMANAGER_H
#pragma once

#include <Arduino.h>

class Logger;

class HttpOtaService {
public:
    static void begin(Logger *logger,
                      const char *manifestUrl,
                      const char *device,
                      const char *currentVersion,
                      const char *caCertPemOrNull = nullptr);

    static void loop();

    static void checkNow();

    static void setIntervalMs(uint32_t intervalMs);

    static bool checkForUpdate(String &errorOut, bool &updateAvailable, String &versionOut);

    static bool applyPendingUpdate(String &errorOut);

    static bool hasPendingUpdate(String &versionOut);

private:
    static bool fetchManifest(String &outJson);

    static bool processManifestAndMaybeUpdate(const String &json);

    static bool verifyManifestSignature(const String &device,
                                        const String &version,
                                        const String &appUrl,
                                        const String &appSha256Hex,
                                        const String &fsLabel,
                                        const String &fsUrl,
                                        const String &fsSha256Hex,
                                        const String &sigB64,
                                        const String &kid);

    static bool downloadVerifyAndFlash(const String &url,
                                       const String &expectedSha256Hex,
                                       int updateCommand,
                                       const char *partitionLabel);

    static bool verifyEcdsaP256Signature(const String &payload,
                                         const String &sigB64,
                                         const char *pubKeyPem);

    static int compareVersions(const String &a, const String &b);

    static void clearPendingUpdate();
    static void loadAppliedHashes();
    static void storeAppliedAppHash(const String &hash);
    static void storeAppliedFsHash(const String &hash);

    static Logger *s_logger;
    static const char *s_manifestUrl;
    static const char *s_device;
    static const char *s_currentVersion;
    static const char *s_caCertPem;

    static uint32_t s_intervalMs;
    static uint32_t s_lastCheckMs;
    static bool s_forceCheck;

    static bool s_updateAvailable;
    static bool s_pendingUpdateApp;
    static bool s_pendingUpdateFs;
    static String s_pendingVersion;
    static String s_pendingAppUrl;
    static String s_pendingAppSha256;
    static String s_pendingFsLabel;
    static String s_pendingFsUrl;
    static String s_pendingFsSha256;
    static String s_pendingKid;
    static String s_appliedAppSha;
    static String s_appliedFsSha;
    static String s_lastError;
};
#endif //MODBUS_TO_MQTT_HTTPOTAMANAGER_H
