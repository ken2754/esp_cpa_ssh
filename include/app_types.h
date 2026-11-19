#pragma once

#include <stddef.h>
#include <stdint.h>
#include <functional>

namespace esp_cpa_ssh {

// -----------------------------------------------------------------------------
// Hardware & Display Constants (M5Stack Cardputer-ADV, ST7789V2 240x135)
// -----------------------------------------------------------------------------
static constexpr int SCREEN_WIDTH = 240;
static constexpr int SCREEN_HEIGHT = 135;

// 6x8 bitmap font: 40x15 terminal grid (240x120) + 15px status bar (y: 120..134)
static constexpr int FONT_WIDTH = 6;
static constexpr int FONT_HEIGHT = 8;
static constexpr int TERM_COLS = 40;
static constexpr int TERM_ROWS = 15;
static constexpr int STATUS_BAR_Y = TERM_ROWS * FONT_HEIGHT;  // 120
static constexpr int STATUS_BAR_HEIGHT = SCREEN_HEIGHT - STATUS_BAR_Y;  // 15

// Cardputer-ADV SD Card SPI Pins
static constexpr int SD_SPI_SCK_PIN = 40;
static constexpr int SD_SPI_MISO_PIN = 39;
static constexpr int SD_SPI_MOSI_PIN = 14;
static constexpr int SD_SPI_CS_PIN = 12;

// -----------------------------------------------------------------------------
// Configuration & Profile Limits
// -----------------------------------------------------------------------------
static constexpr size_t MAX_WIFI_PROFILES = 3;
static constexpr size_t MAX_SSH_PROFILES = 3;

static constexpr size_t WIFI_SSID_BUF_LEN = 33;  // 32 + NUL
static constexpr size_t WIFI_PASS_BUF_LEN = 65;  // 64 + NUL
static constexpr size_t MAX_LABEL_LEN = 24;
static constexpr size_t MAX_HOST_LEN = 64;
static constexpr size_t MAX_USER_LEN = 32;
static constexpr size_t MAX_KEY_PATH_LEN = 96;
static constexpr size_t MAX_SECRET_LEN = 64;

static constexpr const char* DEFAULT_KEY_PATH = "/id_ed25519";
static constexpr const char* CONFIG_FILE_PATH = "/config.json";
static constexpr const char* KNOWN_HOSTS_PATH = "/known_hosts";

struct WifiProfile {
    char ssid[WIFI_SSID_BUF_LEN] = {0};
    char password[WIFI_PASS_BUF_LEN] = {0};
    bool configured = false;
};

enum SshAuthType : uint8_t {
    AUTH_ED25519 = 0,
    AUTH_PASSWORD = 1,
};

struct SshProfile {
    char label[MAX_LABEL_LEN] = {0};
    char host[MAX_HOST_LEN] = {0};
    uint16_t port = 22;
    char username[MAX_USER_LEN] = {0};
    SshAuthType auth_type = AUTH_ED25519;
    char key_path[MAX_KEY_PATH_LEN] = {0};   // e.g. "/id_ed25519" on LittleFS or "/sd/id_ed25519"
    char secret[MAX_SECRET_LEN] = {0};       // Password (AUTH_PASSWORD) or key passphrase (AUTH_ED25519)
    bool configured = false;
};

struct AppConfig {
    WifiProfile wifi[MAX_WIFI_PROFILES];
    SshProfile ssh[MAX_SSH_PROFILES];
    int8_t last_wifi_idx = 0;
    int8_t last_ssh_idx = 0;
};

// -----------------------------------------------------------------------------
// Application State & Events
// -----------------------------------------------------------------------------
enum class AppState : uint8_t {
    BOOT,
    MENU_MAIN,
    MENU_WIFI,
    MENU_SSH,
    CONNECTING_WIFI,
    CONNECTING_SSH,
    TERMINAL_ACTIVE,
    ERROR_PROMPT,
};

enum class SshState : uint8_t {
    DISCONNECTED = 0,
    CONNECTING,
    HOST_VERIFY,
    AUTHENTICATING,
    CONNECTED,
    ERROR,
};

struct StatusInfo {
    bool wifi_connected = false;
    int8_t rssi = 0;
    SshState ssh_state = SshState::DISCONNECTED;
    char active_host[MAX_LABEL_LEN] = {0};
    bool fn_active = false;
    bool ctrl_active = false;
    bool alt_active = false;
    uint8_t battery_pct = 0;
};

// -----------------------------------------------------------------------------
// Module Interfaces (for parallel development without tight coupling)
// -----------------------------------------------------------------------------
using TerminalDataCallback = std::function<void(const uint8_t* data, size_t len)>;
using SshStatusCallback = std::function<void(SshState state, const char* msg)>;
using HostVerifyCallback = std::function<bool(const char* host, uint16_t port, const char* key_type, const char* sha256_fp, bool is_mismatch)>;

class IConfigStore {
public:
    virtual ~IConfigStore() = default;
    virtual bool begin() = 0;
    virtual bool loadConfig(AppConfig& out) = 0;
    virtual bool saveConfig(const AppConfig& cfg) = 0;
    virtual bool loadPrivateKey(const char* path, char* out_buf, size_t max_len, size_t& out_len) = 0;
    virtual bool importKeyFromSd(const char* sd_path, const char* lfs_path) = 0;
    virtual bool checkKnownHost(const char* host, uint16_t port, const char* key_type, const char* sha256_fp, bool& out_mismatch) = 0;
    virtual bool saveKnownHost(const char* host, uint16_t port, const char* key_type, const char* sha256_fp) = 0;
};

class IWifiManager {
public:
    virtual ~IWifiManager() = default;
    virtual bool connect(const WifiProfile& profile, uint32_t timeout_ms = 15000) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual int8_t rssi() const = 0;
    virtual void localIp(char* buf, size_t len) const = 0;
};

class ISshClient {
public:
    virtual ~ISshClient() = default;
    virtual void setDataCallback(TerminalDataCallback cb) = 0;
    virtual void setStatusCallback(SshStatusCallback cb) = 0;
    virtual void setHostVerifyCallback(HostVerifyCallback cb) = 0;
    virtual bool connectAsync(const SshProfile& profile, const char* private_key_pem) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual SshState state() const = 0;
    virtual bool sendBytes(const uint8_t* data, size_t len) = 0;
    virtual bool sendWindowSize(uint16_t cols, uint16_t rows) = 0;
};

class ITerminalView {
public:
    virtual ~ITerminalView() = default;
    virtual void begin() = 0;
    virtual void clear() = 0;
    virtual void feed(const uint8_t* data, size_t len) = 0;
    virtual void renderIfDirty(const StatusInfo& status) = 0;
    virtual void scrollUp(int lines) = 0;
    virtual void scrollDown(int lines) = 0;
};

}  // namespace esp_cpa_ssh
