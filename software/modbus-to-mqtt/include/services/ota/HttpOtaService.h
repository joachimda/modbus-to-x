#ifndef MODBUS_TO_MQTT_HTTPOTAMANAGER_H
#define MODBUS_TO_MQTT_HTTPOTAMANAGER_H
#pragma once

#include <Arduino.h>
#include <atomic>

class Logger;

class HttpOtaService {
public:
    using ProgressCallback = void (*)(const char *stage,
                                      uint32_t received,
                                      uint32_t total,
                                      const char *detail);

    static void begin(Logger *logger,
                      const char *manifestUrl,
                      const char *device,
                      const char *currentVersion,
                      const char *caCertPemOrNull = nullptr);

    static void loop();

    static void checkNow();

    static void setIntervalMs(uint32_t intervalMs);

    static void setProgressCallback(ProgressCallback cb);

    static bool checkForUpdate(String &errorOut, bool &updateAvailable, String &versionOut);

    static bool applyPendingUpdate(String &errorOut);

    static bool hasPendingUpdate(String &versionOut);

    static bool isCheckPending();

    static void getLastCheckStatus(bool &ok, bool &available, String &versionOut, String &errorOut);

    static void requestReleaseNotes();

    static void getNotesStatus(bool &ready, bool &pending, String &notesOut, String &errorOut);

private:
    static void emitProgress(const char *stage,
                             uint32_t received,
                             uint32_t total,
                             const char *detail);

    static bool fetchManifest(String &outJson);

    static bool processManifestAndMaybeUpdate(const String &json);

    static bool verifyManifestSignature(const String &device,
                                        const String &version,
                                        const String &appUrl,
                                        const String &appSha256Hex,
                                        const String &fsLabel,
                                        const String &fsUrl,
                                        const String &fsSha256Hex,
                                        const String &notesUrl,
                                        const String &notesSha256Hex,
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
    static bool fetchReleaseNotes(String &outText);

    static Logger *s_logger;
    static const char *s_manifestUrl;
    static const char *s_device;
    static const char *s_currentVersion;
    static const char *s_caCertPem;

    static ProgressCallback s_progressCb;

    static uint32_t s_intervalMs;
    static uint32_t s_lastCheckMs;
    static bool s_forceCheck;
    static std::atomic<bool> s_checkRequested;
    static std::atomic<bool> s_checkInProgress;

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
    static String s_pendingNotesUrl;
    static String s_pendingNotesSha256;
    static String s_pendingNotesText;
    static bool s_notesReady;
    static std::atomic<bool> s_notesRequested;
    static std::atomic<bool> s_notesInProgress;
    static String s_lastNotesError;
    static String s_appliedAppSha;
    static String s_appliedFsSha;
    static String s_lastError;
    static bool s_lastCheckOk;
    static bool s_lastCheckAvailable;
    static String s_lastCheckVersion;
    static String s_lastCheckError;
};
#endif //MODBUS_TO_MQTT_HTTPOTAMANAGER_H
