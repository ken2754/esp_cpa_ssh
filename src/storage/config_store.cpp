#include "config_store.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

#include <cstdio>
#include <cstring>

namespace esp_cpa_ssh {

namespace {

class SdMountGuard {
public:
    SdMountGuard() : spi_(HSPI) {
        spi_.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
        mounted_ = SD.begin(SD_SPI_CS_PIN, spi_);
    }

    ~SdMountGuard() {
        if (mounted_) {
            SD.end();
        }
        spi_.end();
    }

    SdMountGuard(const SdMountGuard&) = delete;
    SdMountGuard& operator=(const SdMountGuard&) = delete;

    bool isMounted() const { return mounted_; }

private:
    SPIClass spi_;
    bool mounted_ = false;
};

SshAuthType parseAuthType(JsonVariantConst v) {
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (s != nullptr && (strcasecmp(s, "password") == 0 || strcmp(s, "1") == 0)) {
            return AUTH_PASSWORD;
        }
        return AUTH_ED25519;
    }
    if (v.is<int>()) {
        return v.as<int>() == static_cast<int>(AUTH_PASSWORD) ? AUTH_PASSWORD : AUTH_ED25519;
    }
    return AUTH_ED25519;
}

bool parseConfigJson(const JsonDocument& doc, AppConfig& out) {
    out = AppConfig{};

    int wifi_idx = doc["last_wifi_idx"] | 0;
    if (wifi_idx < 0 || wifi_idx >= static_cast<int>(MAX_WIFI_PROFILES)) {
        wifi_idx = 0;
    }
    out.last_wifi_idx = static_cast<int8_t>(wifi_idx);

    int ssh_idx = doc["last_ssh_idx"] | 0;
    if (ssh_idx < 0 || ssh_idx >= static_cast<int>(MAX_SSH_PROFILES)) {
        ssh_idx = 0;
    }
    out.last_ssh_idx = static_cast<int8_t>(ssh_idx);

    if (doc["wifi"].is<JsonArrayConst>()) {
        JsonArrayConst wifi_arr = doc["wifi"].as<JsonArrayConst>();
        size_t i = 0;
        for (JsonVariantConst item : wifi_arr) {
            if (i >= MAX_WIFI_PROFILES) {
                break;
            }
            if (!item.is<JsonObjectConst>()) {
                ++i;
                continue;
            }
            JsonObjectConst obj = item.as<JsonObjectConst>();
            const char* ssid = obj["ssid"] | "";
            const char* pass = obj["password"] | "";
            strlcpy(out.wifi[i].ssid, ssid, sizeof(out.wifi[i].ssid));
            strlcpy(out.wifi[i].password, pass, sizeof(out.wifi[i].password));
            if (obj["configured"].is<bool>()) {
                out.wifi[i].configured = obj["configured"].as<bool>();
            } else {
                out.wifi[i].configured = (out.wifi[i].ssid[0] != '\0');
            }
            ++i;
        }
    }

    if (doc["ssh"].is<JsonArrayConst>()) {
        JsonArrayConst ssh_arr = doc["ssh"].as<JsonArrayConst>();
        size_t i = 0;
        for (JsonVariantConst item : ssh_arr) {
            if (i >= MAX_SSH_PROFILES) {
                break;
            }
            if (!item.is<JsonObjectConst>()) {
                ++i;
                continue;
            }
            JsonObjectConst obj = item.as<JsonObjectConst>();
            const char* label = obj["label"] | "";
            const char* host = obj["host"] | "";
            uint16_t port = obj["port"] | static_cast<uint16_t>(22);
            if (port == 0) {
                port = 22;
            }
            const char* username = obj["username"] | (obj["user"] | "");
            SshAuthType auth_type = parseAuthType(obj["auth_type"]);
            const char* key_path = obj["key_path"] | "";
            const char* secret = obj["secret"] | (obj["password"] | (obj["passphrase"] | ""));

            strlcpy(out.ssh[i].label, label, sizeof(out.ssh[i].label));
            strlcpy(out.ssh[i].host, host, sizeof(out.ssh[i].host));
            out.ssh[i].port = port;
            strlcpy(out.ssh[i].username, username, sizeof(out.ssh[i].username));
            out.ssh[i].auth_type = auth_type;

            if (key_path[0] != '\0') {
                strlcpy(out.ssh[i].key_path, key_path, sizeof(out.ssh[i].key_path));
            } else if (auth_type == AUTH_ED25519) {
                strlcpy(out.ssh[i].key_path, DEFAULT_KEY_PATH, sizeof(out.ssh[i].key_path));
            }

            strlcpy(out.ssh[i].secret, secret, sizeof(out.ssh[i].secret));

            if (obj["configured"].is<bool>()) {
                out.ssh[i].configured = obj["configured"].as<bool>();
            } else {
                out.ssh[i].configured = (out.ssh[i].host[0] != '\0' && out.ssh[i].username[0] != '\0');
            }
            ++i;
        }
    }

    return true;
}

bool replaceLittleFsFile(const char* tmp_path, const char* final_path) {
    if (LittleFS.exists(final_path)) {
        if (!LittleFS.remove(final_path)) {
            LittleFS.remove(tmp_path);
            return false;
        }
    }
    if (!LittleFS.rename(tmp_path, final_path)) {
        LittleFS.remove(tmp_path);
        return false;
    }
    return true;
}

}  // namespace

bool ConfigStore::begin() {
    if (mounted_) {
        return true;
    }
    if (!LittleFS.begin(true)) {
        return false;
    }
    if (!LittleFS.exists(KEYS_DIR_PATH)) {
        LittleFS.mkdir(KEYS_DIR_PATH);
    }
    mounted_ = true;
    return true;
}

bool ConfigStore::ensureLittleFs() {
    return mounted_ || begin();
}

void ConfigStore::normalizePath(const char* input, char* out, size_t out_len, const char* fallback) {
    if (out_len == 0) {
        return;
    }
    const char* src = (input != nullptr && input[0] != '\0') ? input : fallback;
    if (src == nullptr || src[0] == '\0') {
        out[0] = '\0';
        return;
    }
    if (src[0] == '/') {
        strlcpy(out, src, out_len);
    } else {
        snprintf(out, out_len, "/%s", src);
    }
}

void ConfigStore::ensureParentDirs(const char* lfs_path) {
    if (lfs_path == nullptr || lfs_path[0] != '/') {
        return;
    }
    char dir_buf[MAX_KEY_PATH_LEN] = {0};
    strlcpy(dir_buf, lfs_path, sizeof(dir_buf));
    for (size_t i = 1; dir_buf[i] != '\0'; ++i) {
        if (dir_buf[i] == '/') {
            dir_buf[i] = '\0';
            if (!LittleFS.exists(dir_buf)) {
                LittleFS.mkdir(dir_buf);
            }
            dir_buf[i] = '/';
        }
    }
}

void ConfigStore::formatHostPort(const char* host, uint16_t port, char* out, size_t out_len) {
    if (host == nullptr) {
        host = "";
    }
    if (port == 0) {
        port = 22;
    }
    snprintf(out, out_len, "%s:%u", host, static_cast<unsigned>(port));
}

bool ConfigStore::loadConfig(AppConfig& out) {
    out = AppConfig{};
    if (!ensureLittleFs()) {
        return false;
    }
    if (!LittleFS.exists(CONFIG_FILE_PATH)) {
        return false;
    }

    File file = LittleFS.open(CONFIG_FILE_PATH, "r");
    if (!file) {
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
        return false;
    }

    return parseConfigJson(doc, out);
}

bool ConfigStore::saveConfig(const AppConfig& cfg) {
    if (!ensureLittleFs()) {
        return false;
    }

    JsonDocument doc;
    doc["last_wifi_idx"] = cfg.last_wifi_idx;
    doc["last_ssh_idx"] = cfg.last_ssh_idx;

    JsonArray wifi_arr = doc["wifi"].to<JsonArray>();
    for (size_t i = 0; i < MAX_WIFI_PROFILES; ++i) {
        JsonObject obj = wifi_arr.add<JsonObject>();
        obj["ssid"] = cfg.wifi[i].ssid;
        obj["password"] = cfg.wifi[i].password;
        obj["configured"] = cfg.wifi[i].configured;
    }

    JsonArray ssh_arr = doc["ssh"].to<JsonArray>();
    for (size_t i = 0; i < MAX_SSH_PROFILES; ++i) {
        JsonObject obj = ssh_arr.add<JsonObject>();
        obj["label"] = cfg.ssh[i].label;
        obj["host"] = cfg.ssh[i].host;
        obj["port"] = cfg.ssh[i].port;
        obj["username"] = cfg.ssh[i].username;
        obj["auth_type"] = static_cast<uint8_t>(cfg.ssh[i].auth_type);
        obj["key_path"] = cfg.ssh[i].key_path;
        obj["secret"] = cfg.ssh[i].secret;
        obj["configured"] = cfg.ssh[i].configured;
    }

    static constexpr const char* kTmpConfigPath = "/config.json.tmp";
    File file = LittleFS.open(kTmpConfigPath, "w");
    if (!file) {
        return false;
    }

    size_t written = serializeJson(doc, file);
    file.close();
    if (written == 0) {
        LittleFS.remove(kTmpConfigPath);
        return false;
    }

    return replaceLittleFsFile(kTmpConfigPath, CONFIG_FILE_PATH);
}

bool ConfigStore::loadPrivateKey(const char* path, char* out_buf, size_t max_len, size_t& out_len) {
    out_len = 0;
    if (out_buf == nullptr || max_len < 2) {
        return false;
    }
    out_buf[0] = '\0';

    if (!ensureLittleFs()) {
        return false;
    }

    char norm_path[MAX_KEY_PATH_LEN] = {0};
    normalizePath(path, norm_path, sizeof(norm_path), DEFAULT_KEY_PATH);

    File file;
    if (LittleFS.exists(norm_path)) {
        file = LittleFS.open(norm_path, "r");
    } else if (strncmp(norm_path, "/keys/", 6) != 0) {
        char keys_fallback[MAX_KEY_PATH_LEN] = {0};
        snprintf(keys_fallback, sizeof(keys_fallback), "%s%s", KEYS_DIR_PATH, norm_path);
        if (LittleFS.exists(keys_fallback)) {
            file = LittleFS.open(keys_fallback, "r");
        }
    }

    if (!file) {
        return false;
    }

    size_t pos = 0;
    bool prev_cr = false;
    while (file.available()) {
        int ch = file.read();
        if (ch < 0) {
            break;
        }
        char c = static_cast<char>(ch);
        if (c == '\r') {
            if (pos >= max_len - 1) {
                file.close();
                out_buf[0] = '\0';
                out_len = 0;
                return false;
            }
            out_buf[pos++] = '\n';
            prev_cr = true;
            continue;
        }
        if (c == '\n' && prev_cr) {
            prev_cr = false;
            continue;
        }
        prev_cr = false;
        if (pos >= max_len - 1) {
            file.close();
            out_buf[0] = '\0';
            out_len = 0;
            return false;
        }
        out_buf[pos++] = c;
    }
    file.close();

    out_buf[pos] = '\0';
    out_len = pos;
    return pos > 0;
}

bool ConfigStore::importKeyFromSd(const char* sd_path, const char* lfs_path) {
    if (!ensureLittleFs()) {
        return false;
    }

    char norm_sd[MAX_KEY_PATH_LEN] = {0};
    char norm_lfs[MAX_KEY_PATH_LEN] = {0};
    normalizePath(sd_path, norm_sd, sizeof(norm_sd), DEFAULT_KEY_PATH);
    normalizePath(lfs_path, norm_lfs, sizeof(norm_lfs), DEFAULT_KEY_PATH);

    SdMountGuard sd;
    if (!sd.isMounted()) {
        return false;
    }
    if (!SD.exists(norm_sd)) {
        return false;
    }

    File src = SD.open(norm_sd, FILE_READ);
    if (!src) {
        return false;
    }

    size_t src_size = src.size();
    if (src_size == 0 || src_size > MAX_PRIVATE_KEY_BYTES) {
        src.close();
        return false;
    }

    ensureParentDirs(norm_lfs);

    char tmp_lfs[MAX_KEY_PATH_LEN + 8] = {0};
    snprintf(tmp_lfs, sizeof(tmp_lfs), "%s.tmp", norm_lfs);

    File dst = LittleFS.open(tmp_lfs, "w");
    if (!dst) {
        src.close();
        return false;
    }

    size_t written = 0;
    bool prev_cr = false;
    while (src.available()) {
        int ch = src.read();
        if (ch < 0) {
            break;
        }
        uint8_t b = static_cast<uint8_t>(ch);
        if (b == '\r') {
            if (dst.write(static_cast<uint8_t>('\n')) != 1) {
                src.close();
                dst.close();
                LittleFS.remove(tmp_lfs);
                return false;
            }
            ++written;
            prev_cr = true;
            continue;
        }
        if (b == '\n' && prev_cr) {
            prev_cr = false;
            continue;
        }
        prev_cr = false;
        if (dst.write(b) != 1) {
            src.close();
            dst.close();
            LittleFS.remove(tmp_lfs);
            return false;
        }
        ++written;
    }

    src.close();
    dst.close();

    if (written == 0) {
        LittleFS.remove(tmp_lfs);
        return false;
    }

    return replaceLittleFsFile(tmp_lfs, norm_lfs);
}

bool ConfigStore::importConfigFromSd(AppConfig& out, const char* sd_path) {
    if (!ensureLittleFs()) {
        return false;
    }

    char norm_sd[MAX_KEY_PATH_LEN] = {0};
    normalizePath(sd_path, norm_sd, sizeof(norm_sd), SD_CONFIG_IMPORT_PATH);

    SdMountGuard sd;
    if (!sd.isMounted()) {
        return false;
    }
    if (!SD.exists(norm_sd)) {
        return false;
    }

    File src = SD.open(norm_sd, FILE_READ);
    if (!src) {
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, src);
    src.close();
    if (err) {
        return false;
    }

    AppConfig imported;
    if (!parseConfigJson(doc, imported)) {
        return false;
    }

    out = imported;
    return saveConfig(out);
}

bool ConfigStore::checkKnownHost(const char* host, uint16_t port, const char* key_type, const char* sha256_fp, bool& out_mismatch) {
    out_mismatch = false;
    if (host == nullptr || host[0] == '\0' || sha256_fp == nullptr || sha256_fp[0] == '\0') {
        return false;
    }
    if (!ensureLittleFs() || !LittleFS.exists(KNOWN_HOSTS_PATH)) {
        return false;
    }

    File file = LittleFS.open(KNOWN_HOSTS_PATH, "r");
    if (!file) {
        return false;
    }

    char target_hp[MAX_HOST_LEN + 16] = {0};
    formatHostPort(host, port, target_hp, sizeof(target_hp));
    const char* expected_type = (key_type != nullptr && key_type[0] != '\0') ? key_type : "*";

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.isEmpty() || line.startsWith("#")) {
            continue;
        }

        int sp1 = line.indexOf(' ');
        if (sp1 <= 0) {
            continue;
        }
        int sp2 = line.indexOf(' ', sp1 + 1);
        if (sp2 <= sp1 + 1) {
            continue;
        }

        String entry_hp = line.substring(0, sp1);
        String entry_type = line.substring(sp1 + 1, sp2);
        String entry_fp = line.substring(sp2 + 1);
        entry_fp.trim();

        if (entry_hp == target_hp) {
            bool type_ok = (entry_type == expected_type) || (entry_type == "*") || (strcmp(expected_type, "*") == 0);
            bool fp_ok = (entry_fp == sha256_fp);
            file.close();
            if (type_ok && fp_ok) {
                out_mismatch = false;
                return true;
            }
            out_mismatch = true;
            return false;
        }
    }

    file.close();
    return false;
}

bool ConfigStore::saveKnownHost(const char* host, uint16_t port, const char* key_type, const char* sha256_fp) {
    if (host == nullptr || host[0] == '\0' || sha256_fp == nullptr || sha256_fp[0] == '\0') {
        return false;
    }
    if (!ensureLittleFs()) {
        return false;
    }

    char target_hp[MAX_HOST_LEN + 16] = {0};
    formatHostPort(host, port, target_hp, sizeof(target_hp));
    const char* safe_type = (key_type != nullptr && key_type[0] != '\0') ? key_type : "*";

    static constexpr const char* kTmpKnownHosts = "/known_hosts.tmp";
    File dst = LittleFS.open(kTmpKnownHosts, "w");
    if (!dst) {
        return false;
    }

    if (LittleFS.exists(KNOWN_HOSTS_PATH)) {
        File src = LittleFS.open(KNOWN_HOSTS_PATH, "r");
        if (src) {
            while (src.available()) {
                String line = src.readStringUntil('\n');
                line.trim();
                if (line.isEmpty()) {
                    continue;
                }
                int sp1 = line.indexOf(' ');
                if (sp1 > 0 && line.substring(0, sp1) == target_hp) {
                    continue;  // Replace existing entry for this host:port
                }
                dst.println(line);
            }
            src.close();
        }
    }

    dst.printf("%s %s %s\n", target_hp, safe_type, sha256_fp);
    dst.close();

    return replaceLittleFsFile(kTmpKnownHosts, KNOWN_HOSTS_PATH);
}

bool ConfigStore::hasPrivateKey(const char* path) {
    if (!ensureLittleFs()) {
        return false;
    }
    char norm_path[MAX_KEY_PATH_LEN] = {0};
    normalizePath(path, norm_path, sizeof(norm_path), DEFAULT_KEY_PATH);
    if (LittleFS.exists(norm_path)) {
        return true;
    }
    if (strncmp(norm_path, "/keys/", 6) != 0) {
        char keys_fallback[MAX_KEY_PATH_LEN] = {0};
        snprintf(keys_fallback, sizeof(keys_fallback), "%s%s", KEYS_DIR_PATH, norm_path);
        return LittleFS.exists(keys_fallback);
    }
    return false;
}

bool ConfigStore::deletePrivateKey(const char* path) {
    if (!ensureLittleFs()) {
        return false;
    }
    char norm_path[MAX_KEY_PATH_LEN] = {0};
    normalizePath(path, norm_path, sizeof(norm_path), DEFAULT_KEY_PATH);
    if (LittleFS.exists(norm_path)) {
        return LittleFS.remove(norm_path);
    }
    return false;
}

bool ConfigStore::clearKnownHosts() {
    if (!ensureLittleFs()) {
        return false;
    }
    if (!LittleFS.exists(KNOWN_HOSTS_PATH)) {
        return true;
    }
    return LittleFS.remove(KNOWN_HOSTS_PATH);
}

}  // namespace esp_cpa_ssh
