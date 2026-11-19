#pragma once

#include "app_types.h"

namespace esp_cpa_ssh {

static constexpr const char* KEYS_DIR_PATH = "/keys";
static constexpr const char* SD_CONFIG_IMPORT_PATH = "/cardputer_ssh.json";
static constexpr size_t MAX_PRIVATE_KEY_BYTES = 16384;

class ConfigStore : public IConfigStore {
public:
    ConfigStore() = default;
    ~ConfigStore() override = default;

    bool begin() override;
    bool loadConfig(AppConfig& out) override;
    bool saveConfig(const AppConfig& cfg) override;
    bool loadPrivateKey(const char* path, char* out_buf, size_t max_len, size_t& out_len) override;
    bool importKeyFromSd(const char* sd_path, const char* lfs_path) override;
    bool checkKnownHost(const char* host, uint16_t port, const char* key_type, const char* sha256_fp, bool& out_mismatch) override;
    bool saveKnownHost(const char* host, uint16_t port, const char* key_type, const char* sha256_fp) override;

    // Additional helpers for UI / SD import
    bool importConfigFromSd(AppConfig& out, const char* sd_path = SD_CONFIG_IMPORT_PATH);
    bool hasPrivateKey(const char* path = DEFAULT_KEY_PATH);
    bool deletePrivateKey(const char* path);
    bool clearKnownHosts();
    bool isMounted() const { return mounted_; }

private:
    bool mounted_ = false;

    bool ensureLittleFs();
    static void normalizePath(const char* input, char* out, size_t out_len, const char* fallback);
    static void ensureParentDirs(const char* lfs_path);
    static void formatHostPort(const char* host, uint16_t port, char* out, size_t out_len);
};

}  // namespace esp_cpa_ssh
