#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Update.h>

#include <ArduinoJson.h>

#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

#include "services/ota/HttpOtaService.h"
#include "services/ota/OtaPublicKeys.h"
#include "services/IndicatorService.h"
#include "Logger.h"
#include "esp_partition.h"
#include <Preferences.h>

Logger *HttpOtaService::s_logger = nullptr;
const char* HttpOtaService::s_manifestUrl = nullptr;
const char* HttpOtaService::s_device = nullptr;
const char* HttpOtaService::s_currentVersion = nullptr;
const char* HttpOtaService::s_caCertPem = nullptr;

uint32_t HttpOtaService::s_intervalMs = 6UL * 60UL * 60UL * 1000UL; // 6 hours
uint32_t HttpOtaService::s_lastCheckMs = 0;
bool HttpOtaService::s_forceCheck = false;
std::atomic<bool> HttpOtaService::s_checkRequested{false};
std::atomic<bool> HttpOtaService::s_checkInProgress{false};
bool HttpOtaService::s_updateAvailable = false;
bool HttpOtaService::s_pendingUpdateApp = false;
bool HttpOtaService::s_pendingUpdateFs = false;
String HttpOtaService::s_pendingVersion;
String HttpOtaService::s_pendingAppUrl;
String HttpOtaService::s_pendingAppSha256;
String HttpOtaService::s_pendingFsLabel;
String HttpOtaService::s_pendingFsUrl;
String HttpOtaService::s_pendingFsSha256;
String HttpOtaService::s_pendingKid;
String HttpOtaService::s_pendingNotesUrl;
String HttpOtaService::s_pendingNotesSha256;
String HttpOtaService::s_pendingNotesText;
bool HttpOtaService::s_notesReady = false;
std::atomic<bool> HttpOtaService::s_notesRequested{false};
std::atomic<bool> HttpOtaService::s_notesInProgress{false};
String HttpOtaService::s_lastNotesError;
String HttpOtaService::s_appliedAppSha;
String HttpOtaService::s_appliedFsSha;
String HttpOtaService::s_lastError;
bool HttpOtaService::s_lastCheckOk = false;
bool HttpOtaService::s_lastCheckAvailable = false;
String HttpOtaService::s_lastCheckVersion;
String HttpOtaService::s_lastCheckError;

static void logInfo(const Logger *logger, const char* msg) {
    if (logger) logger->logInformation(msg);
    Serial.println(msg);
}
static void logWarn(const Logger *logger, const char* msg) {
    if (logger) logger->logWarning(msg);
    Serial.println(msg);
}
static void logErr(const Logger *logger, const char* msg) {
    if (logger) logger->logError(msg);
    Serial.println(msg);
}

static bool getPartitionSizeBytes(const char *label, size_t &outSize) {
    outSize = 0;
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, label);
    if (!part) return false;
    outSize = part->size;
    return true;
}

void HttpOtaService::begin(Logger* logger,
                           const char* manifestUrl,
                           const char* device,
                           const char* currentVersion,
                           const char* caCertPemOrNull) {
    s_logger = logger;
    s_manifestUrl = manifestUrl;
    s_device = device;
    s_currentVersion = currentVersion;
    s_caCertPem = caCertPemOrNull;

    s_lastCheckMs = 0;
    s_forceCheck = true;
    loadAppliedHashes();

    if (s_logger) {
        s_logger->logInformation("HTTP-OTA: Ready");
    }
}

void HttpOtaService::setIntervalMs(const uint32_t intervalMs) {
    s_intervalMs = intervalMs;
}

void HttpOtaService::checkNow() {
    s_forceCheck = true;
    s_checkRequested.store(true, std::memory_order_release);
}

void HttpOtaService::loop() {
    if (!s_manifestUrl || s_manifestUrl[0] == '\0' || !s_device || !s_currentVersion) return;

    const uint32_t now = millis();
    const bool timeToCheck = now - s_lastCheckMs >= s_intervalMs;

    const bool shouldCheck = s_forceCheck || timeToCheck || s_checkRequested.load(std::memory_order_acquire);
    if (shouldCheck && !s_checkInProgress.load(std::memory_order_acquire)) {
        if (WiFiClass::status() != WL_CONNECTED) {
            if (s_checkRequested.load(std::memory_order_acquire)) {
                s_lastCheckOk = false;
                s_lastCheckAvailable = false;
                s_lastCheckVersion = "";
                s_lastCheckError = "wifi_disconnected";
                s_checkRequested.store(false, std::memory_order_release);
            }
        } else {
            s_forceCheck = false;
            s_checkRequested.store(false, std::memory_order_release);
            s_checkInProgress.store(true, std::memory_order_release);

            String err;
            bool available = false;
            String version;
            const bool ok = checkForUpdate(err, available, version);
            s_lastCheckOk = ok;
            s_lastCheckAvailable = available;
            s_lastCheckVersion = version;
            s_lastCheckError = err;
            s_checkInProgress.store(false, std::memory_order_release);
        }
    }

    const bool wantsNotes = s_notesRequested.load(std::memory_order_acquire);
    if (!wantsNotes || s_notesInProgress.load(std::memory_order_acquire)) return;
    if (!s_updateAvailable) {
        s_lastNotesError = "no_update";
        s_notesRequested.store(false, std::memory_order_release);
        return;
    }
    if (WiFiClass::status() != WL_CONNECTED) {
        s_lastNotesError = "wifi_disconnected";
        s_notesRequested.store(false, std::memory_order_release);
        return;
    }

    s_notesInProgress.store(true, std::memory_order_release);
    String notes;
    const bool notesOk = fetchReleaseNotes(notes);
    if (notesOk) {
        s_pendingNotesText = notes;
        s_notesReady = true;
        s_lastNotesError = "";
    } else {
        s_pendingNotesText = "";
        s_notesReady = false;
        if (s_lastNotesError.isEmpty()) {
            s_lastNotesError = "notes_fetch_failed";
        }
    }
    s_notesRequested.store(false, std::memory_order_release);
    s_notesInProgress.store(false, std::memory_order_release);
}

bool HttpOtaService::checkForUpdate(String &errorOut, bool &updateAvailable, String &versionOut) {
    errorOut = "";
    updateAvailable = false;
    versionOut = "";

    s_forceCheck = false;
    s_lastCheckMs = millis();

    if (!s_manifestUrl || s_manifestUrl[0] == '\0') {
        s_lastError = "manifest_url_unset";
        errorOut = s_lastError;
        return false;
    }
    if (!s_device || s_device[0] == '\0') {
        s_lastError = "device_unset";
        errorOut = s_lastError;
        return false;
    }
    if (WiFiClass::status() != WL_CONNECTED) {
        s_lastError = "wifi_disconnected";
        errorOut = s_lastError;
        return false;
    }

    String manifestJson;
    if (!fetchManifest(manifestJson)) {
        errorOut = s_lastError;
        return false;
    }

    if (!processManifestAndMaybeUpdate(manifestJson)) {
        errorOut = s_lastError;
        return false;
    }

    updateAvailable = s_updateAvailable;
    if (s_updateAvailable) versionOut = s_pendingVersion;
    return true;
}

bool HttpOtaService::isCheckPending() {
    return s_checkRequested.load(std::memory_order_acquire)
        || s_checkInProgress.load(std::memory_order_acquire);
}

void HttpOtaService::getLastCheckStatus(bool &ok, bool &available, String &versionOut, String &errorOut) {
    ok = s_lastCheckOk;
    available = s_lastCheckAvailable;
    versionOut = s_lastCheckVersion;
    errorOut = s_lastCheckError;
}

void HttpOtaService::requestReleaseNotes() {
    if (!s_updateAvailable) return;
    if (s_notesReady) return;
    s_lastNotesError = "";
    s_notesRequested.store(true, std::memory_order_release);
}

void HttpOtaService::getNotesStatus(bool &ready, bool &pending, String &notesOut, String &errorOut) {
    ready = s_notesReady;
    pending = s_notesRequested.load(std::memory_order_acquire)
        || s_notesInProgress.load(std::memory_order_acquire);
    notesOut = s_pendingNotesText;
    errorOut = s_lastNotesError;
}

bool HttpOtaService::applyPendingUpdate(String &errorOut) {
    errorOut = "";
    if (!s_updateAvailable) {
        errorOut = "no_update";
        s_lastError = errorOut;
        return false;
    }
    if (!s_pendingUpdateApp && !s_pendingUpdateFs) {
        errorOut = "no_components";
        s_lastError = errorOut;
        return false;
    }
    if (WiFiClass::status() != WL_CONNECTED) {
        errorOut = "wifi_disconnected";
        s_lastError = errorOut;
        return false;
    }

    logInfo(s_logger, "HTTP-OTA: Applying pending update");
    IndicatorService::instance().setOtaActive(true);
    bool ok = true;
    if (s_pendingUpdateFs) {
        logInfo(s_logger, "HTTP-OTA: Updating filesystem");
        ok = downloadVerifyAndFlash(s_pendingFsUrl, s_pendingFsSha256, U_SPIFFS, s_pendingFsLabel.c_str());
        if (ok) {
            storeAppliedFsHash(s_pendingFsSha256);
        }
    } else {
        logInfo(s_logger, "HTTP-OTA: Filesystem up to date, skipping");
    }
    if (ok) {
        if (s_pendingUpdateApp) {
            logInfo(s_logger, "HTTP-OTA: Updating firmware");
            ok = downloadVerifyAndFlash(s_pendingAppUrl, s_pendingAppSha256, U_FLASH, nullptr);
            if (ok) {
                storeAppliedAppHash(s_pendingAppSha256);
            }
        } else {
            logInfo(s_logger, "HTTP-OTA: Firmware up to date, skipping");
        }
    }
    IndicatorService::instance().setOtaActive(false);

    if (!ok) {
        errorOut = "apply_failed";
        s_lastError = errorOut;
        return false;
    }

    clearPendingUpdate();
    return true;
}

bool HttpOtaService::hasPendingUpdate(String &versionOut) {
    versionOut = "";
    if (!s_updateAvailable || (!s_pendingUpdateApp && !s_pendingUpdateFs)) return false;
    versionOut = s_pendingVersion;
    return true;
}

bool HttpOtaService::fetchManifest(String& outJson) {
    WiFiClientSecure client;
    if (s_caCertPem && s_caCertPem[0] != '\0') {
        client.setCACert(s_caCertPem);
    } else {
        // todo: fetch up to date CA certs
        client.setInsecure();
    }

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, s_manifestUrl)) {
        logErr(s_logger, "HTTP-OTA: http.begin() failed");
        s_lastError = "http_begin_failed";
        return false;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        const String msg = "HTTP-OTA: Manifest GET failed: " + String(code);
        logErr(s_logger, msg.c_str());
        s_lastError = "manifest_http_error";
        http.end();
        return false;
    }

    outJson = http.getString();
    http.end();
    return true;
}

bool HttpOtaService::processManifestAndMaybeUpdate(const String& json) {
    JsonDocument doc;

    auto err = deserializeJson(doc, json);
    if (err) {
        logErr(s_logger, "HTTP-OTA: Manifest JSON parse failed");
        s_lastError = "manifest_parse_failed";
        return false;
    }

    const String device = doc["device"] | "";
    const String version = doc["version"] | "";
    const String kid = doc["kid"] | "";
    const String sig = doc["sig"] | "";
    const String appUrl = doc["app"]["url"] | "";
    const String appSha256 = doc["app"]["sha256"] | "";
    const String fsLabel = doc["fs"]["label"] | "";
    const String fsUrl = doc["fs"]["url"] | "";
    const String fsSha256 = doc["fs"]["sha256"] | "";
    const String notesUrl = doc["notes"]["url"] | "";
    const String notesSha256 = doc["notes"]["sha256"] | "";

    if (device.isEmpty() || version.isEmpty() || appUrl.isEmpty() ||
        appSha256.length() < 64 || sig.isEmpty()) {
        logErr(s_logger, "HTTP-OTA: Manifest missing required fields");
        s_lastError = "manifest_missing_fields";
        return false;
    }

    if (fsLabel.isEmpty() || fsUrl.isEmpty() || fsSha256.length() < 64) {
        logErr(s_logger, "HTTP-OTA: Manifest missing filesystem fields");
        s_lastError = "manifest_missing_filesystem";
        return false;
    }

    if (notesUrl.isEmpty() || notesSha256.length() < 64) {
        logErr(s_logger, "HTTP-OTA: Manifest missing notes fields");
        s_lastError = "manifest_missing_notes";
        return false;
    }

    if (kid.isEmpty()) {
        logErr(s_logger, "HTTP-OTA: Manifest missing KID");
        s_lastError = "manifest_missing_kid";
        return false;
    }

    if (fsLabel != "spiffs") {
        logErr(s_logger, "HTTP-OTA: Filesystem label mismatch (expected spiffs)");
        s_lastError = "filesystem_label_mismatch";
        return false;
    }

    if (!s_device || device != s_device) {
        logErr(s_logger, "HTTP-OTA: Manifest device mismatch");
        s_lastError = "device_mismatch";
        return false;
    }

    if (!verifyManifestSignature(device, version, appUrl, appSha256, fsLabel, fsUrl, fsSha256,
                                 notesUrl, notesSha256, sig, kid)) {
        logErr(s_logger, "HTTP-OTA: Signature verification failed");
        s_lastError = "signature_invalid";
        return false;
    }

    // Only update if the manifest version is newer than the current
    if (compareVersions(version, String(s_currentVersion)) <= 0) {
        logInfo(s_logger, "HTTP-OTA: No update available");
        clearPendingUpdate();
        return true;
    }

    s_pendingUpdateApp = true;
    s_pendingUpdateFs = true;
    if (s_appliedAppSha.length() >= 64 && s_appliedAppSha.equalsIgnoreCase(appSha256)) {
        s_pendingUpdateApp = false;
    }
    if (s_appliedFsSha.length() >= 64 && s_appliedFsSha.equalsIgnoreCase(fsSha256)) {
        s_pendingUpdateFs = false;
    }
    if (!s_pendingUpdateApp && !s_pendingUpdateFs) {
        logInfo(s_logger, "HTTP-OTA: Update available but no component changes");
        clearPendingUpdate();
        return true;
    }

    s_updateAvailable = true;
    s_pendingVersion = version;
    s_pendingAppUrl = appUrl;
    s_pendingAppSha256 = appSha256;
    s_pendingFsLabel = fsLabel;
    s_pendingFsUrl = fsUrl;
    s_pendingFsSha256 = fsSha256;
    s_pendingKid = kid;
    s_pendingNotesUrl = notesUrl;
    s_pendingNotesSha256 = notesSha256;
    s_pendingNotesText = "";
    s_notesReady = false;
    s_lastNotesError = "";
    s_lastError = "";
    logInfo(s_logger, "HTTP-OTA: Update available");
    return true;
}

bool HttpOtaService::verifyManifestSignature(const String& device,
                                             const String& version,
                                             const String& appUrl,
                                             const String& appSha256Hex,
                                             const String& fsLabel,
                                             const String& fsUrl,
                                             const String& fsSha256Hex,
                                             const String& notesUrl,
                                             const String& notesSha256Hex,
                                             const String& sigB64,
                                             const String& kid) {
    String payload = device + "|" + version + "|" + appSha256Hex + "|" + appUrl + "|" +
        fsLabel + "|" + fsSha256Hex + "|" + fsUrl + "|" + kid + "|" + notesSha256Hex + "|" + notesUrl;

    const char *pubKeyPem = nullptr;
    if (!kid.isEmpty()) {
        pubKeyPem = ota_find_pubkey_pem_by_kid(kid.c_str());
    }
    if (!pubKeyPem && kid.isEmpty() && OTA_PUBKEYS_COUNT == 1) {
        pubKeyPem = OTA_PUBKEYS[0].pem;
    }

    if (!pubKeyPem) {
        logErr(s_logger, "HTTP-OTA: No matching public key for manifest KID");
        return false;
    }

    return verifyEcdsaP256Signature(payload, sigB64, pubKeyPem);
}

bool HttpOtaService::verifyEcdsaP256Signature(const String& payload,
                                              const String& sigB64,
                                              const char *pubKeyPem) {
    if (!pubKeyPem || pubKeyPem[0] == '\0') {
        logErr(s_logger, "HTTP-OTA: Missing public key");
        return false;
    }

    // Decode base64 signature
    size_t sigLen = 0;
    // ECDSA signature (DER-encoded) size varies; 128 is safe for P-256 DER signatures
    uint8_t sigBuf[128];

    const int b64rc = mbedtls_base64_decode(sigBuf, sizeof(sigBuf), &sigLen,
                                           reinterpret_cast<const unsigned char *>(sigB64.c_str()),
                                           sigB64.length());
    if (b64rc != 0 || sigLen == 0) {
        logErr(s_logger, "HTTP-OTA: base64 decode sig failed");
        return false;
    }

    // Parse public key
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    const int pkrc = mbedtls_pk_parse_public_key(&pk,
                        (const unsigned char*)pubKeyPem,
                        strlen(pubKeyPem) + 1);
    if (pkrc != 0) {
        mbedtls_pk_free(&pk);
        logErr(s_logger, "HTTP-OTA: parse public key failed");
        return false;
    }

    // SHA-256(payload)
    unsigned char hash[32];
    const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!mdInfo || mbedtls_md(mdInfo, (const unsigned char*)payload.c_str(), payload.length(), hash) != 0) {
        mbedtls_pk_free(&pk);
        logErr(s_logger, "HTTP-OTA: sha256 hash failed");
        return false;
    }

    const int vrc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
                                      hash, sizeof(hash),
                                      sigBuf, sigLen);

    mbedtls_pk_free(&pk);
    return vrc == 0;
}

bool HttpOtaService::downloadVerifyAndFlash(const String& url,
                                            const String& expectedSha256Hex,
                                            const int updateCommand,
                                            const char *partitionLabel) {
    WiFiClientSecure client;
    const uint32_t httpTimeoutMs = updateCommand == U_FLASH ? 120000 : 30000;
    client.setTimeout(httpTimeoutMs);
    if (s_caCertPem && s_caCertPem[0] != '\0') {
        client.setCACert(s_caCertPem);
    } else {
        client.setInsecure(); // supply CA for production
    }

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    http.setTimeout(httpTimeoutMs);

    if (!http.begin(client, url)) {
        logErr(s_logger, "HTTP-OTA: http.begin() firmware failed");
        return false;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        String msg = "HTTP-OTA: Firmware GET failed: " + String(code);
        logErr(s_logger, msg.c_str());
        http.end();
        return false;
    }

    const int totalLen = http.getSize();
    const size_t totalSize = totalLen > 0 ? static_cast<size_t>(totalLen) : 0;
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        logErr(s_logger, "HTTP-OTA: No HTTP stream available");
        s_lastError = "http_stream_missing";
        http.end();
        return false;
    }
    stream->setTimeout(httpTimeoutMs);

    {
        char msg[160];
        const char *kind = updateCommand == U_FLASH ? "firmware" : "filesystem";
        if (totalLen > 0) {
            snprintf(msg, sizeof(msg),
                     "HTTP-OTA: Download size %ld bytes (%s)",
                     static_cast<long>(totalLen), kind);
        } else {
            snprintf(msg, sizeof(msg),
                     "HTTP-OTA: Download size unknown (%s)", kind);
        }
        logInfo(s_logger, msg);
    }

    if (updateCommand == U_SPIFFS && partitionLabel && partitionLabel[0] != '\0') {
        size_t partSize = 0;
        if (!getPartitionSizeBytes(partitionLabel, partSize)) {
            logErr(s_logger, "HTTP-OTA: Filesystem partition not found");
            s_lastError = "fs_partition_not_found";
            http.end();
            return false;
        }
        if (totalLen > 0) {
            const auto imgSize = static_cast<size_t>(totalLen);
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "HTTP-OTA: FS image size %lu bytes, partition %s size %lu bytes",
                     static_cast<unsigned long>(imgSize), partitionLabel,
                     static_cast<unsigned long>(partSize));
            logInfo(s_logger, msg);
            if (imgSize > partSize) {
                logErr(s_logger, "HTTP-OTA: FS image larger than partition, aborting");
                s_lastError = "fs_image_too_large";
                http.end();
                return false;
            }
            if (imgSize != partSize) {
                logWarn(s_logger, "HTTP-OTA: FS image size differs from partition size");
            }
        } else {
            logWarn(s_logger, "HTTP-OTA: FS image size unknown (no content-length)");
        }
    }

    // Begin OTA update
    const size_t updateSize = totalLen > 0 ? (size_t)totalLen : UPDATE_SIZE_UNKNOWN;
    bool started = false;
    if (partitionLabel && partitionLabel[0] != '\0') {
        started = Update.begin(updateSize, updateCommand, -1, LOW, partitionLabel);
    } else {
        started = Update.begin(updateSize, updateCommand);
    }

    if (!started) {
        logErr(s_logger, "HTTP-OTA: Update.begin failed");
        http.end();
        return false;
    }

    // Hash as we write
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);

    uint8_t buf[2048];
    const uint32_t stallTimeoutMs = updateCommand == U_FLASH ? 120000 : 20000;
    uint32_t lastDataMs = millis();
    uint32_t lastYieldMs = lastDataMs;
    uint32_t lastReportMs = lastDataMs;
    size_t received = 0;

    while (http.connected() && (totalSize == 0 || received < totalSize)) {
        const int avail = stream->available();
        if (avail <= 0) {
            // safety timeout if the stream stalls
            if (static_cast<uint32_t>(millis() - lastDataMs) > stallTimeoutMs) {
                char msg[160];
                const char *kind = updateCommand == U_FLASH ? "firmware" : "filesystem";
                snprintf(msg, sizeof(msg),
                         "HTTP-OTA: %s download timeout after %lu bytes",
                         kind, static_cast<unsigned long>(received));
                logErr(s_logger, msg);
                Update.abort();
                http.end();
                mbedtls_sha256_free(&sha);
                return false;
            }
            delay(1);
            continue;
        }

        const int toRead = avail > static_cast<int>(sizeof(buf))
        ? static_cast<int>(sizeof(buf)) : avail;
        const int r = stream->readBytes(buf, toRead);
        if (r <= 0) break;

        lastDataMs = millis();
        if (lastDataMs - lastYieldMs > 50) {
            delay(1);
            lastYieldMs = lastDataMs;
        }
        mbedtls_sha256_update_ret(&sha, buf, r);

        const size_t written = Update.write(buf, r);
        if (written != static_cast<size_t>(r)) {
            logErr(s_logger, "HTTP-OTA: Update.write failed");
            Update.abort();
            http.end();
            mbedtls_sha256_free(&sha);
            return false;
        }
        received += static_cast<size_t>(r);
        if (lastDataMs - lastReportMs > 5000) {
            char msg[160];
            if (totalLen > 0) {
                snprintf(msg, sizeof(msg),
                         "HTTP-OTA: Downloaded %lu/%lu bytes",
                         static_cast<unsigned long>(received),
                         static_cast<unsigned long>(totalLen));
            } else {
                snprintf(msg, sizeof(msg),
                         "HTTP-OTA: Downloaded %lu bytes",
                         static_cast<unsigned long>(received));
            }
            logInfo(s_logger, msg);
            lastReportMs = lastDataMs;
        }
    }

    unsigned char digest[32];
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);

    // Convert to hex
    char hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(hex + i * 2, "%02x", digest[i]);
    }
    hex[64] = '\0';

    if (!expectedSha256Hex.equalsIgnoreCase(hex)) {
        logErr(s_logger, "HTTP-OTA: SHA256 mismatch");
        Update.abort();
        http.end();
        return false;
    }

    if (!Update.end(true)) {
        logErr(s_logger, "HTTP-OTA: Update.end failed");
        http.end();
        return false;
    }

    http.end();
    return true;
}

bool HttpOtaService::fetchReleaseNotes(String &outText) {
    outText = "";
    if (s_pendingNotesUrl.isEmpty() || s_pendingNotesSha256.length() < 64) {
        s_lastNotesError = "notes_missing";
        logErr(s_logger, "HTTP-OTA: Notes URL/hash missing");
        return false;
    }

    constexpr size_t kMaxNotesBytes = 65536;
    logInfo(s_logger, "HTTP-OTA: Fetching release notes");
    WiFiClientSecure client;
    client.setTimeout(20000);
    if (s_caCertPem && s_caCertPem[0] != '\0') {
        client.setCACert(s_caCertPem);
    } else {
        client.setInsecure();
    }

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    http.setTimeout(20000);

    if (!http.begin(client, s_pendingNotesUrl)) {
        s_lastNotesError = "notes_http_begin_failed";
        logErr(s_logger, "HTTP-OTA: Notes http.begin() failed");
        return false;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        s_lastNotesError = "notes_http_error";
        String msg = "HTTP-OTA: Notes GET failed: " + String(code);
        logErr(s_logger, msg.c_str());
        http.end();
        return false;
    }

    const int totalLen = http.getSize();
    if (totalLen > 0 && static_cast<size_t>(totalLen) > kMaxNotesBytes) {
        s_lastNotesError = "notes_too_large";
        logErr(s_logger, "HTTP-OTA: Notes too large");
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    if (!stream) {
        s_lastNotesError = "notes_stream_missing";
        logErr(s_logger, "HTTP-OTA: Notes stream missing");
        http.end();
        return false;
    }
    stream->setTimeout(20000);

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);

    if (totalLen > 0) {
        outText.reserve(static_cast<size_t>(totalLen));
    }

    uint8_t buf[512];
    size_t received = 0;
    uint32_t lastDataMs = millis();
    const uint32_t stallTimeoutMs = 15000;
    const size_t totalSize = totalLen > 0 ? static_cast<size_t>(totalLen) : 0;

    while (http.connected() && (totalSize == 0 || received < totalSize)) {
        const int avail = stream->available();
        if (avail <= 0) {
            if ((uint32_t)(millis() - lastDataMs) > stallTimeoutMs) {
                s_lastNotesError = "notes_timeout";
                logErr(s_logger, "HTTP-OTA: Notes download timeout");
                mbedtls_sha256_free(&sha);
                http.end();
                return false;
            }
            delay(1);
            continue;
        }

        const int toRead = (avail > (int)sizeof(buf)) ? (int)sizeof(buf) : avail;
        const int r = stream->readBytes(buf, toRead);
        if (r <= 0) break;

        lastDataMs = millis();
        mbedtls_sha256_update_ret(&sha, buf, r);
        if (!outText.concat(reinterpret_cast<const char *>(buf), r)) {
            s_lastNotesError = "notes_oom";
            logErr(s_logger, "HTTP-OTA: Notes buffer overflow");
            mbedtls_sha256_free(&sha);
            http.end();
            return false;
        }
        received += static_cast<size_t>(r);
        if (received > kMaxNotesBytes) {
            s_lastNotesError = "notes_too_large";
            logErr(s_logger, "HTTP-OTA: Notes too large");
            mbedtls_sha256_free(&sha);
            http.end();
            return false;
        }
    }

    unsigned char digest[32];
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);

    char hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(hex + i * 2, "%02x", digest[i]);
    }
    hex[64] = '\0';

    if (!s_pendingNotesSha256.equalsIgnoreCase(hex)) {
        s_lastNotesError = "notes_sha_mismatch";
        logErr(s_logger, "HTTP-OTA: Notes SHA256 mismatch");
        http.end();
        return false;
    }

    http.end();
    return true;
}

// Simple semver-ish compare (major.minor.patch), ignores suffixes like -beta.1
// Returns -1 if a<b, 0 if equal, 1 if a>b
int HttpOtaService::compareVersions(const String& a, const String& b) {
    Serial.println("Current Version: " + b);
    Serial.println("New Version: " + a);
    auto parse3 = [](const String& v, int& maj, int& min, int& pat) {

        maj = min = pat = 0;

        // strip anything after '-' (e.g., 1.2.3-beta)
        String core = v;
        const int dash = core.indexOf('-');
        if (dash >= 0) core = core.substring(0, dash);
        while (core.length() > 0 && (core[0] == 'v' || core[0] == 'V')) {
            core.remove(0, 1);
        }

        const int p1 = core.indexOf('.');
        const int p2 = p1 >= 0 ? core.indexOf('.', p1 + 1) : -1;

        const String sMaj = p1 >= 0 ? core.substring(0, p1) : core;
        const String sMin = p1 >= 0 && p2 >= 0 ? core.substring(p1 + 1, p2) : p1 >= 0 ? core.substring(p1 + 1) : "0";
        const String sPat = p2 >= 0 ? core.substring(p2 + 1) : "0";

        maj = sMaj.toInt();
        min = sMin.toInt();
        pat = sPat.toInt();
    };

    int aM, am, ap, bM, bm, bp;
    parse3(a, aM, am, ap);
    parse3(b, bM, bm, bp);

    if (aM != bM) return aM < bM ? -1 : 1;
    if (am != bm) return am < bm ? -1 : 1;
    if (ap != bp) return ap < bp ? -1 : 1;
    return 0;
}

void HttpOtaService::clearPendingUpdate() {
    s_updateAvailable = false;
    s_pendingUpdateApp = false;
    s_pendingUpdateFs = false;
    s_pendingVersion = "";
    s_pendingAppUrl = "";
    s_pendingAppSha256 = "";
    s_pendingFsLabel = "";
    s_pendingFsUrl = "";
    s_pendingFsSha256 = "";
    s_pendingKid = "";
    s_pendingNotesUrl = "";
    s_pendingNotesSha256 = "";
    s_pendingNotesText = "";
    s_notesReady = false;
    s_notesRequested.store(false, std::memory_order_release);
    s_notesInProgress.store(false, std::memory_order_release);
    s_lastNotesError = "";
    s_lastError = "";
}

void HttpOtaService::loadAppliedHashes() {
    Preferences prefs;
    if (!prefs.begin("ota", true)) return;
    s_appliedAppSha = prefs.getString("app_sha", "");
    s_appliedFsSha = prefs.getString("fs_sha", "");
    prefs.end();
}

void HttpOtaService::storeAppliedAppHash(const String &hash) {
    if (hash.length() < 64) return;
    Preferences prefs;
    if (!prefs.begin("ota", false)) return;
    prefs.putString("app_sha", hash);
    prefs.end();
    s_appliedAppSha = hash;
}

void HttpOtaService::storeAppliedFsHash(const String &hash) {
    if (hash.length() < 64) return;
    Preferences prefs;
    if (!prefs.begin("ota", false)) return;
    prefs.putString("fs_sha", hash);
    prefs.end();
    s_appliedFsSha = hash;
}
