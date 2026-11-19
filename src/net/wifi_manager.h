#pragma once

#include "app_types.h"

namespace esp_cpa_ssh {

class WifiManager : public IWifiManager {
public:
    WifiManager() = default;
    ~WifiManager() override = default;

    bool connect(const WifiProfile& profile, uint32_t timeout_ms = 15000) override;
    void disconnect() override;
    bool isConnected() const override;
    int8_t rssi() const override;
    void localIp(char* buf, size_t len) const override;
};

}  // namespace esp_cpa_ssh
