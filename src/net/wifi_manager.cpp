#include "wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cstdio>
#include <cstring>

namespace esp_cpa_ssh {

bool WifiManager::connect(const WifiProfile& profile, uint32_t timeout_ms) {
    if (profile.ssid[0] == '\0') {
        return false;
    }

    if (WiFi.status() == WL_CONNECTED && strcmp(WiFi.SSID().c_str(), profile.ssid) == 0) {
        return true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.disconnect(false);
    delay(50);

    const char* pass = (profile.password[0] != '\0') ? profile.password : nullptr;
    WiFi.begin(profile.ssid, pass);

    const uint32_t start_ms = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start_ms >= timeout_ms) {
            WiFi.disconnect(true);
            return false;
        }
        delay(100);
    }

    return true;
}

void WifiManager::disconnect() {
    WiFi.disconnect(true);
}

bool WifiManager::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

int8_t WifiManager::rssi() const {
    if (!isConnected()) {
        return 0;
    }
    return static_cast<int8_t>(WiFi.RSSI());
}

void WifiManager::localIp(char* buf, size_t len) const {
    if (!buf || len == 0) {
        return;
    }
    if (!isConnected()) {
        snprintf(buf, len, "0.0.0.0");
        return;
    }
    IPAddress ip = WiFi.localIP();
    snprintf(buf, len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

}  // namespace esp_cpa_ssh
