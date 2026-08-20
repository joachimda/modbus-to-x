#include "network/wifi/WifiConnectionController.h"

#include <cstring>

#include "network/wifi/WifiCredentialFieldCore.h"
#include "network/wifi/WifiConfigurationStorage.h"
#include "services/TimeService.h"

namespace {

WifiConnectionState toPublicState(const ProvisioningAttemptCore::State state) {
    switch (state) {
        case ProvisioningAttemptCore::State::Idle: return WifiConnectionState::Idle;
        case ProvisioningAttemptCore::State::Connecting: return WifiConnectionState::Connecting;
        case ProvisioningAttemptCore::State::Connected: return WifiConnectionState::Connected;
        case ProvisioningAttemptCore::State::Failed: return WifiConnectionState::Failed;
        case ProvisioningAttemptCore::State::Disconnected: return WifiConnectionState::Disconnected;
    }
    return WifiConnectionState::Failed;
}

bool configureAddressing(const WifiStaticConfig &configuration) {
    if (!configuration.any()) {
        return WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }

    IPAddress ip;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns1;
    IPAddress dns2;
    if (!ip.fromString(configuration.ip)
        || !gateway.fromString(configuration.gateway)
        || !subnet.fromString(configuration.subnet)
        || (configuration.dns1.length() && !dns1.fromString(configuration.dns1))
        || (configuration.dns2.length() && !dns2.fromString(configuration.dns2))) {
        return false;
    }
    return WiFi.config(ip, gateway, subnet, dns1, dns2);
}

}  // namespace

void WifiConnectionController::begin(const String &hostname) {
    const std::lock_guard<std::mutex> guard(_operationMutex);
    Serial.printf("WiFiConnectController::begin(%s) called\n", hostname.c_str());
    removeEventHandler();
    _hostname = hostname;
    WiFi.persistent(false);
    WiFiClass::mode(WIFI_STA);
    WiFi.disconnect(true, false);
    _attempt.reset();
    _eventGate.reset();
    _pendingConnection.clear();
    _activeToken = 0U;
    _eventHandlerId = WiFi.onEvent([this](const WiFiEvent_t event, const WiFiEventInfo_t info) {
        onEvent(event, info);
    });
    _eventHandlerRegistered = true;
    Serial.println("WiFiConnectController::begin() ended");
}

void WifiConnectionController::reset() {
    const std::lock_guard<std::mutex> guard(_operationMutex);
    removeEventHandler();
    WiFi.disconnect(true, true);
    _attempt.reset();
    _eventGate.reset();
    _pendingConnection.clear();
    _activeToken = 0U;
}

void WifiConnectionController::setApChannelIfNeeded(const uint8_t channel) {
    Serial.printf("WiFiConnectController::setApChannelIfNeeded(%d) called\n", channel);
    if (!channel) return;
    wifi_config_t wifiConfiguration{};
    if (esp_wifi_get_config(WIFI_IF_AP, &wifiConfiguration) == ESP_OK
        && wifiConfiguration.ap.channel != channel) {
        wifiConfiguration.ap.channel = channel;
        (void)esp_wifi_set_config(WIFI_IF_AP, &wifiConfiguration);
    }
}

bool WifiConnectionController::connect(const String &ssid, const String &password, const String &bssidText,
                                       const WifiStaticConfig &staticConfiguration, const bool save,
                                       const uint8_t channel) {
    const std::lock_guard<std::mutex> guard(_operationMutex);
    if (_attempt.isConnecting()) return false;

    WiFi.setAutoReconnect(false);
    setApChannelIfNeeded(channel);

    AttemptCore::CandidateValue candidate;
    candidate.ssid = ssid;
    candidate.password = password;
    candidate.networkConfiguration = staticConfiguration;
    candidate.save = save;
    const AttemptToken token = _attempt.start(std::move(candidate));
    _activeToken = token;
    _deadline = millis() + _timeoutMs;
    _pendingConnection.ssid = ssid;
    _pendingConnection.password = password;
    _pendingConnection.bssid = bssidText;
    _pendingConnection.staticConfiguration = staticConfiguration;
    _pendingConnection.channel = channel;

    // Arduino dispatches driver events through a separate FIFO. Stop STA while
    // leaving the setup AP running and wait for STA_STOP in that FIFO before
    // arming this attempt. Events queued by the previous connection are thereby
    // drained before the new driver connection can produce any events.
    _eventGate.awaitStationStop(token);
    wifi_mode_t mode = WiFiClass::getMode();
    if ((mode & WIFI_MODE_AP) == 0) {
        mode = static_cast<wifi_mode_t>(mode | WIFI_MODE_AP);
        if (!WiFiClass::mode(mode)) {
            fail(token, "EVENT_BARRIER_FAILED");
            return true;
        }
    }
    if ((mode & WIFI_MODE_STA) == 0) {
        mode = static_cast<wifi_mode_t>(mode | WIFI_MODE_STA);
        if (!WiFiClass::mode(mode)) {
            fail(token, "EVENT_BARRIER_FAILED");
            return true;
        }
    }
    const wifi_mode_t apOnlyMode = static_cast<wifi_mode_t>(mode & ~WIFI_MODE_STA);
    if (!WiFiClass::mode(apOnlyMode)) fail(token, "EVENT_BARRIER_FAILED");
    return true;
}

void WifiConnectionController::loop() {
    const std::lock_guard<std::mutex> guard(_operationMutex);
    if (_attempt.isConnecting()
        && static_cast<int32_t>(millis() - _deadline) >= 0) {
        const AttemptToken token = _activeToken;
        fail(token, "TIMEOUT");
        WiFi.disconnect(false, false);
    }
}

WifiStatus WifiConnectionController::getStatus() const {
    const auto snapshot = _attempt.snapshot();
    WifiStatus result;
    result.state = toPublicState(snapshot.state);
    result.ssid = snapshot.ssid;
    result.ip = snapshot.ip;
    result.reason = snapshot.reason;
    result.hasIp = snapshot.hasIp;
    result.provisioningReady = snapshot.provisioningReady;
    return result;
}

void WifiConnectionController::cancel() {
    const std::lock_guard<std::mutex> guard(_operationMutex);
    if (!_attempt.isConnecting()) return;
    const AttemptToken token = _activeToken;
    (void)_attempt.cancel(token);
    _eventGate.cancel(token);
    _pendingConnection.clear();
    WiFi.disconnect(false, false);
}

void WifiConnectionController::onEvent(const arduino_event_id_t event, const arduino_event_info_t &info) {
    const std::lock_guard<std::mutex> guard(_operationMutex);
    const auto decision = _eventGate.onEvent(toGateEvent(event));
    if (decision.action == ProvisioningEventGate::Action::StartConnection) {
        startConnection(decision.token);
        return;
    }
    if (decision.action != ProvisioningEventGate::Action::Deliver) return;
    const AttemptToken token = decision.token;
    if (!_attempt.isCurrent(token)) return;

    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("WiFiConnectController::onEvent - station connected");
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
            Serial.println("WiFiConnectController::onEvent - station got IP");
            AttemptCore::CandidateValue candidate;
            const String stationIp = IPAddress(info.got_ip.ip_info.ip.addr).toString();
            const auto result = _attempt.gotIp(token, stationIp, candidate);
            if (result == ProvisioningAttemptCore::GotIpResult::Stale) return;

            TimeService::requestSync();
            WiFi.setAutoReconnect(true);
            if (result == ProvisioningAttemptCore::GotIpResult::PersistenceRequired) {
                // Verify the DHCP/static record before writing credentials. If
                // network storage fails, a new credential candidate must not be
                // left partially committed in the ESP-IDF Wi-Fi store.
                auto persistenceResult = ProvisioningAttemptCore::PersistenceResult::Succeeded;
                if (!WifiConfigurationStorage::save(candidate.networkConfiguration)) {
                    persistenceResult = ProvisioningAttemptCore::PersistenceResult::NetworkConfigurationFailed;
                } else if (!persistCredentials(candidate.ssid, candidate.password)) {
                    persistenceResult = ProvisioningAttemptCore::PersistenceResult::CredentialFailed;
                }
                (void)_attempt.completePersistence(token, persistenceResult);
            }
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            String reason = "DISCONNECTED";
            if (info.wifi_sta_disconnected.reason == WIFI_REASON_NO_AP_FOUND) reason = "NO_AP_FOUND";
            else if (info.wifi_sta_disconnected.reason == WIFI_REASON_AUTH_FAIL) reason = "WRONG_PASSWORD";
            else if (info.wifi_sta_disconnected.reason == WIFI_REASON_BEACON_TIMEOUT) reason = "BEACON_TIMEOUT";
            else if (info.wifi_sta_disconnected.reason == WIFI_REASON_ASSOC_EXPIRE) reason = "ASSOC_EXPIRE";
            else if (info.wifi_sta_disconnected.reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
                reason = "HANDSHAKE_TIMEOUT";
            }
            (void)_attempt.disconnect(token, std::move(reason));
            break;
        }
        default: break;
    }
}

void WifiConnectionController::startConnection(const AttemptToken token) {
    if (!_attempt.isCurrent(token) || !_attempt.isConnecting()) {
        _eventGate.cancel(token);
        _pendingConnection.clear();
        return;
    }
    if (!WiFiClass::mode(WIFI_AP_STA)) {
        fail(token, "BEGIN_FAILED");
        return;
    }
    if (!configureAddressing(_pendingConnection.staticConfiguration)) {
        fail(token, "BAD_STATIC_CONFIG");
        return;
    }
    if (_hostname.length()) WiFiClass::setHostname(_hostname.c_str());

    WiFi.persistent(false);
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
        fail(token, "RAM_STORAGE_FAILED");
        return;
    }

    uint8_t bssid[6]{};
    const bool useBssid = parseBssid(_pendingConnection.bssid, bssid);
    const int stationChannel = _pendingConnection.channel ? _pendingConnection.channel : 0;
    const wl_status_t result = useBssid
                                   ? WiFi.begin(_pendingConnection.ssid.c_str(), _pendingConnection.password.c_str(),
                                                stationChannel, bssid)
                                   : WiFi.begin(_pendingConnection.ssid.c_str(), _pendingConnection.password.c_str(),
                                                stationChannel);
    _pendingConnection.clear();
    if (result == WL_CONNECT_FAILED) fail(token, "BEGIN_FAILED");
}

ProvisioningEventGate::Event WifiConnectionController::toGateEvent(const arduino_event_id_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_STOP: return ProvisioningEventGate::Event::StationStopped;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED: return ProvisioningEventGate::Event::StationConnected;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP: return ProvisioningEventGate::Event::StationGotIp;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: return ProvisioningEventGate::Event::StationDisconnected;
        default: return ProvisioningEventGate::Event::Other;
    }
}

bool WifiConnectionController::parseBssid(const String &text, uint8_t output[6]) {
    if (text.length() != 17) return false;
    int values[6]{};
    if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) output[i] = static_cast<uint8_t>(values[i]);
    return true;
}

void WifiConnectionController::fail(const AttemptToken token, const String &reason) {
    Serial.printf("WiFiConnectController::fail(%s) called\n", reason.c_str());
    if (_attempt.fail(token, reason)) {
        _eventGate.cancel(token);
        _pendingConnection.clear();
    }
}

bool WifiConnectionController::persistCredentials(const String &ssid, const String &password) {
    wifi_config_t configuration{};
    if (!WifiCredentialFieldCore::copy(configuration.sta.ssid, sizeof(configuration.sta.ssid),
                                       ssid.c_str(), ssid.length())
        || !WifiCredentialFieldCore::copy(configuration.sta.password, sizeof(configuration.sta.password),
                                          password.c_str(), password.length())) {
        return false;
    }
    configuration.sta.bssid_set = false;

    const esp_err_t flashResult = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    const esp_err_t writeResult = flashResult == ESP_OK
                                      ? esp_wifi_set_config(WIFI_IF_STA, &configuration)
                                      : flashResult;
    wifi_config_t readback{};
    const esp_err_t readResult = writeResult == ESP_OK
                                     ? esp_wifi_get_config(WIFI_IF_STA, &readback)
                                     : writeResult;
    const esp_err_t ramResult = esp_wifi_set_storage(WIFI_STORAGE_RAM);

    return flashResult == ESP_OK && writeResult == ESP_OK && readResult == ESP_OK && ramResult == ESP_OK
           && WifiCredentialFieldCore::equals(readback.sta.ssid, sizeof(readback.sta.ssid),
                                              ssid.c_str(), ssid.length())
           && WifiCredentialFieldCore::equals(readback.sta.password, sizeof(readback.sta.password),
                                              password.c_str(), password.length());
}

void WifiConnectionController::removeEventHandler() {
    if (!_eventHandlerRegistered) return;
    WiFi.removeEvent(_eventHandlerId);
    _eventHandlerRegistered = false;
    _eventHandlerId = 0U;
}
