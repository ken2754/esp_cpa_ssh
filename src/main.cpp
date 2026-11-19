#include <M5Cardputer.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "app_types.h"
#include "net/ssh_client.h"
#include "net/wifi_manager.h"
#include "storage/config_store.h"
#include "ui/input_handler.h"
#include "ui/menu_ui.h"
#include "ui/terminal_view.h"

using namespace esp_cpa_ssh;

namespace {

ConfigStore g_store;
WifiManager g_wifi;
SshClient g_ssh(&g_store);
TerminalView g_term;
InputHandler g_input;
MenuUi g_menu;

AppConfig g_config;
AppState g_state = AppState::MENU_MAIN;

// Thread-safe SSH status handoff
SemaphoreHandle_t g_status_mutex = nullptr;
SshState g_pending_ssh_state = SshState::DISCONNECTED;
char g_pending_ssh_msg[64] = {0};
bool g_ssh_status_dirty = false;
char g_last_ssh_error[64] = {0};

// Thread-safe TOFU HostVerifyCallback handoff (SSH task -> main loop)
struct HostVerifyRequest {
    bool pending = false;
    bool accepted = false;
    char host[MAX_HOST_LEN] = {0};
    uint16_t port = 22;
    char key_type[32] = {0};
    char sha256_fp[96] = {0};
    bool is_mismatch = false;
};

SemaphoreHandle_t g_hv_mutex = nullptr;
SemaphoreHandle_t g_hv_resp_sem = nullptr;
HostVerifyRequest g_hv_req;

char g_key_buf[MAX_PRIVATE_KEY_BYTES] = {0};
uint32_t g_last_status_poll_ms = 0;

void copyStr(char* dst, size_t dst_len, const char* src) {
    if (!dst || dst_len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

const WifiProfile* activeWifiProfile() {
    int idx = g_config.last_wifi_idx;
    if (idx < 0 || idx >= static_cast<int>(MAX_WIFI_PROFILES)) idx = 0;
    return &g_config.wifi[idx];
}

const SshProfile* activeSshProfile() {
    int idx = g_config.last_ssh_idx;
    if (idx < 0 || idx >= static_cast<int>(MAX_SSH_PROFILES)) idx = 0;
    return &g_config.ssh[idx];
}

void onSshStatus(SshState st, const char* msg) {
    if (!g_status_mutex) return;
    if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_pending_ssh_state = st;
        copyStr(g_pending_ssh_msg, sizeof(g_pending_ssh_msg), msg ? msg : "");
        if (st == SshState::ERROR && msg && msg[0] != '\0') {
            copyStr(g_last_ssh_error, sizeof(g_last_ssh_error), msg);
        }
        g_ssh_status_dirty = true;
        xSemaphoreGive(g_status_mutex);
    }
}

bool onHostVerify(const char* host, uint16_t port, const char* key_type, const char* sha256_fp, bool is_mismatch) {
    if (!g_hv_mutex || !g_hv_resp_sem) return false;

    // Drain any stale response token
    while (xSemaphoreTake(g_hv_resp_sem, 0) == pdTRUE) {
    }

    if (xSemaphoreTake(g_hv_mutex, portMAX_DELAY) != pdTRUE) return false;
    copyStr(g_hv_req.host, sizeof(g_hv_req.host), host);
    g_hv_req.port = port;
    copyStr(g_hv_req.key_type, sizeof(g_hv_req.key_type), key_type);
    copyStr(g_hv_req.sha256_fp, sizeof(g_hv_req.sha256_fp), sha256_fp);
    g_hv_req.is_mismatch = is_mismatch;
    g_hv_req.accepted = false;
    g_hv_req.pending = true;
    xSemaphoreGive(g_hv_mutex);

    // Wait for main loop to show dialog and post result
    xSemaphoreTake(g_hv_resp_sem, portMAX_DELAY);

    bool accepted = false;
    if (xSemaphoreTake(g_hv_mutex, portMAX_DELAY) == pdTRUE) {
        accepted = g_hv_req.accepted;
        xSemaphoreGive(g_hv_mutex);
    }
    return accepted;
}

void handlePendingHostVerify() {
    if (!g_hv_mutex || !g_hv_resp_sem) return;

    HostVerifyRequest local_req;
    if (xSemaphoreTake(g_hv_mutex, 0) == pdTRUE) {
        if (g_hv_req.pending) {
            local_req = g_hv_req;
            g_hv_req.pending = false;
        }
        xSemaphoreGive(g_hv_mutex);
    }

    if (!local_req.pending) return;

    const bool accepted = g_menu.showHostVerifyDialog(
        g_input,
        local_req.host,
        local_req.port,
        local_req.key_type,
        local_req.sha256_fp,
        local_req.is_mismatch);

    if (xSemaphoreTake(g_hv_mutex, portMAX_DELAY) == pdTRUE) {
        g_hv_req.accepted = accepted;
        xSemaphoreGive(g_hv_mutex);
    }
    xSemaphoreGive(g_hv_resp_sem);
}

void startSshConnection() {
    const SshProfile* ssh_prof = activeSshProfile();
    const char* key_pem = nullptr;

    if (ssh_prof->auth_type == AUTH_ED25519) {
        const char* kpath = ssh_prof->key_path[0] ? ssh_prof->key_path : DEFAULT_KEY_PATH;
        size_t key_len = 0;
        if (!g_store.loadPrivateKey(kpath, g_key_buf, sizeof(g_key_buf), key_len)) {
            g_menu.showMessageAndWait(g_input, "Key Load Error", "Missing/invalid key:", kpath, 0xF800);
            g_menu.forceRedraw();
            g_state = AppState::MENU_MAIN;
            return;
        }
        key_pem = g_key_buf;
    }

    g_last_ssh_error[0] = '\0';
    if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_pending_ssh_state = SshState::CONNECTING;
        copyStr(g_pending_ssh_msg, sizeof(g_pending_ssh_msg), "Starting SSH...");
        g_ssh_status_dirty = true;
        xSemaphoreGive(g_status_mutex);
    }

    g_menu.showStatusScreen("Connecting SSH", ssh_prof->host, "Negotiating...", 0x07FF);
    if (!g_ssh.connectAsync(*ssh_prof, key_pem)) {
        g_menu.showMessageAndWait(g_input, "SSH Error", "Failed to start SSH task", nullptr, 0xF800);
        g_menu.forceRedraw();
        g_state = AppState::MENU_MAIN;
        return;
    }

    g_state = AppState::CONNECTING_SSH;
}

void handleSdImport() {
    g_menu.showStatusScreen("SD Import", "Checking microSD...", nullptr, 0x07FF);

    bool cfg_ok = g_store.importConfigFromSd(g_config);
    if (cfg_ok) {
        g_store.saveConfig(g_config);
    }

    const SshProfile* prof = activeSshProfile();
    const char* target_key = (prof->key_path[0] != '\0') ? prof->key_path : DEFAULT_KEY_PATH;
    bool key_ok = g_store.importKeyFromSd(DEFAULT_KEY_PATH, target_key);

    if (cfg_ok && key_ok) {
        g_menu.showMessageAndWait(g_input, "SD Import OK", "Imported config & key", target_key, 0x07E0);
    } else if (key_ok) {
        g_menu.showMessageAndWait(g_input, "SD Import OK", "Imported Ed25519 key to:", target_key, 0x07E0);
    } else if (cfg_ok) {
        g_menu.showMessageAndWait(g_input, "SD Import OK", "Imported cardputer_ssh.json", "No /id_ed25519 on SD", 0x07E0);
    } else {
        g_menu.showMessageAndWait(g_input, "SD Import", "No SD card or files found", "Need /id_ed25519 or JSON", 0xFFE0);
    }
    g_menu.forceRedraw();
}

StatusInfo buildStatusInfo() {
    StatusInfo st;
    st.wifi_connected = g_wifi.isConnected();
    st.rssi = g_wifi.rssi();
    st.ssh_state = g_ssh.state();
    const SshProfile* prof = activeSshProfile();
    const char* label = prof->label[0] ? prof->label : prof->host;
    copyStr(st.active_host, sizeof(st.active_host), label);
    st.fn_active = g_input.fnActive();
    st.ctrl_active = g_input.ctrlActive();
    st.alt_active = g_input.altActive();
    int32_t bat = M5Cardputer.Power.getBatteryLevel();
    if (bat < 0) bat = 0;
    if (bat > 100) bat = 100;
    st.battery_pct = static_cast<uint8_t>(bat);
    return st;
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);
    M5Cardputer.Display.setRotation(1);

    g_status_mutex = xSemaphoreCreateMutex();
    g_hv_mutex = xSemaphoreCreateMutex();
    g_hv_resp_sem = xSemaphoreCreateBinary();

    g_store.begin();
    g_store.loadConfig(g_config);

    g_term.begin();
    g_menu.begin();
    g_menu.resetToMain();

    g_ssh.setDataCallback([](const uint8_t* data, size_t len) {
        g_term.feed(data, len);
    });
    g_ssh.setStatusCallback(onSshStatus);
    g_ssh.setHostVerifyCallback(onHostVerify);

    g_state = AppState::MENU_MAIN;
}

void loop() {
    M5Cardputer.update();

    switch (g_state) {
        case AppState::MENU_MAIN: {
            bool cfg_dirty = false;
            MenuAction act = g_menu.update(g_input, g_config, cfg_dirty);
            if (cfg_dirty) {
                g_store.saveConfig(g_config);
            }

            if (act == MenuAction::CONNECT_SSH) {
                const WifiProfile* wp = activeWifiProfile();
                const SshProfile* sp = activeSshProfile();
                if (!wp->configured || wp->ssid[0] == '\0') {
                    g_menu.showMessageAndWait(g_input, "Wi-Fi Required", "Configure a Wi-Fi profile", "in menu first.", 0xFFE0);
                    g_menu.forceRedraw();
                    break;
                }
                if (!sp->configured || sp->host[0] == '\0' || sp->username[0] == '\0') {
                    g_menu.showMessageAndWait(g_input, "SSH Required", "Configure Host & User", "in SSH Profiles first.", 0xFFE0);
                    g_menu.forceRedraw();
                    break;
                }
                if (g_wifi.isConnected()) {
                    startSshConnection();
                } else {
                    g_state = AppState::CONNECTING_WIFI;
                }
            } else if (act == MenuAction::IMPORT_SD) {
                handleSdImport();
            } else if (act == MenuAction::EXIT_LAUNCHER) {
                g_ssh.disconnect();
                g_wifi.disconnect();
                delay(100);

                const esp_partition_t* running = esp_ota_get_running_partition();
                const esp_partition_t* target = nullptr;

                const esp_partition_t* factory_part = esp_partition_find_first(
                    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
                if (factory_part && factory_part != running) {
                    target = factory_part;
                }

                if (!target) {
                    const esp_partition_t* ota0_part = esp_partition_find_first(
                        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
                    if (ota0_part && ota0_part != running) {
                        target = ota0_part;
                    }
                }

                if (!target) {
                    esp_partition_iterator_t it = esp_partition_find(
                        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
                    while (it) {
                        const esp_partition_t* cand = esp_partition_get(it);
                        if (cand && cand != running && (!target || cand->address < target->address)) {
                            target = cand;
                        }
                        it = esp_partition_next(it);
                    }
                    esp_partition_iterator_release(it);
                }

                if (!target) {
                    const esp_partition_t* next_part = esp_ota_get_next_update_partition(nullptr);
                    if (next_part && next_part != running) {
                        target = next_part;
                    }
                }

                if (target && target != running) {
                    if (target->subtype == ESP_PARTITION_SUBTYPE_APP_TEST) {
                        const esp_partition_t* otadata = esp_partition_find_first(
                            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
                        if (otadata) {
                            esp_partition_erase_range(otadata, 0, otadata->size);
                        }
                    } else {
                        esp_ota_set_boot_partition(target);
                    }
                }
                esp_sleep_enable_timer_wakeup(50000);
                esp_deep_sleep_start();
            }
            break;
        }

        case AppState::CONNECTING_WIFI: {
            const WifiProfile* wp = activeWifiProfile();
            g_menu.showStatusScreen("Connecting Wi-Fi", wp->ssid, "Please wait...", 0x07FF);
            if (g_wifi.connect(*wp, 15000)) {
                startSshConnection();
            } else {
                g_menu.showMessageAndWait(g_input, "Wi-Fi Failed", "Could not connect to:", wp->ssid, 0xF800);
                g_menu.forceRedraw();
                g_state = AppState::MENU_MAIN;
            }
            break;
        }

        case AppState::CONNECTING_SSH: {
            handlePendingHostVerify();

            char dummy_ch = 0;
            MenuKey k = g_input.pollMenuKey(dummy_ch);
            if (k == MenuKey::ESC) {
                g_ssh.disconnect();
                g_menu.forceRedraw();
                g_state = AppState::MENU_MAIN;
                break;
            }

            bool dirty = false;
            SshState st = SshState::CONNECTING;
            char msg[64] = {0};
            if (xSemaphoreTake(g_status_mutex, 0) == pdTRUE) {
                if (g_ssh_status_dirty) {
                    dirty = true;
                    st = g_pending_ssh_state;
                    copyStr(msg, sizeof(msg), g_pending_ssh_msg);
                    g_ssh_status_dirty = false;
                }
                xSemaphoreGive(g_status_mutex);
            }

            if (dirty) {
                if (st == SshState::CONNECTED) {
                    g_term.clear();
                    g_state = AppState::TERMINAL_ACTIVE;
                    break;
                } else if (st == SshState::ERROR || st == SshState::DISCONNECTED) {
                    g_ssh.disconnect();
                    const char* err = g_last_ssh_error[0] ? g_last_ssh_error : (msg[0] ? msg : "Connection closed");
                    g_menu.showMessageAndWait(g_input, "SSH Failed", err, nullptr, 0xF800);
                    g_menu.forceRedraw();
                    g_state = AppState::MENU_MAIN;
                    break;
                } else {
                    g_menu.showStatusScreen("Connecting SSH", activeSshProfile()->host, msg, 0x07FF);
                }
            }
            break;
        }

        case AppState::TERMINAL_ACTIVE: {
            uint8_t tx_buf[64];
            bool menu_req = false;
            int scroll_delta = 0;
            size_t tx_len = g_input.poll(tx_buf, sizeof(tx_buf), menu_req, scroll_delta);

            if (scroll_delta < 0) {
                g_term.scrollUp(-scroll_delta);
            } else if (scroll_delta > 0) {
                g_term.scrollDown(scroll_delta);
            }

            if (tx_len > 0) {
                g_ssh.sendBytes(tx_buf, tx_len);
            }

            if (menu_req) {
                g_ssh.disconnect();
                g_menu.resetToMain();
                g_menu.forceRedraw();
                g_state = AppState::MENU_MAIN;
                break;
            }

            if (!g_ssh.isConnected() && g_ssh.state() != SshState::CONNECTED) {
                g_ssh.disconnect();
                const char* err = g_last_ssh_error[0] ? g_last_ssh_error : "Session closed";
                g_menu.showMessageAndWait(g_input, "SSH Disconnected", err, nullptr, 0xFFE0);
                g_menu.resetToMain();
                g_menu.forceRedraw();
                g_state = AppState::MENU_MAIN;
                break;
            }

            // Refresh status bar periodically (battery/RSSI) or on dirty terminal
            uint32_t now = millis();
            if (now - g_last_status_poll_ms >= 500 || tx_len > 0) {
                g_last_status_poll_ms = now;
            }
            g_term.renderIfDirty(buildStatusInfo());
            break;
        }

        default:
            g_state = AppState::MENU_MAIN;
            break;
    }

    delay(5);
}
