#pragma once

#include "app_types.h"
#include "input_handler.h"
#include <M5Cardputer.h>
#include <stddef.h>
#include <stdint.h>

namespace esp_cpa_ssh {

enum class MenuAction : uint8_t {
    NONE = 0,
    CONNECT_SSH,
    IMPORT_SD,
    EXIT_LAUNCHER,
};

class MenuUi {
public:
    MenuUi() = default;

    void begin();
    void forceRedraw() { dirty_ = true; }
    void resetToMain();

    // Non-blocking menu update. Call in loop() when in a menu state.
    MenuAction update(InputHandler& input, AppConfig& cfg, bool& cfg_dirty);

    // Blocking TOFU host key confirmation dialog. Returns true if accepted.
    bool showHostVerifyDialog(InputHandler& input,
                              const char* host,
                              uint16_t port,
                              const char* key_type,
                              const char* sha256_fp,
                              bool is_mismatch);

    // Non-blocking status/progress screen.
    void showStatusScreen(const char* title,
                          const char* line1,
                          const char* line2 = nullptr,
                          uint16_t accent = 0x07FF);

    // Blocking message prompt until user presses any key or BtnA.
    void showMessageAndWait(InputHandler& input,
                            const char* title,
                            const char* line1,
                            const char* line2 = nullptr,
                            uint16_t accent = 0xFFE0);

private:
    enum class Screen : uint8_t {
        MAIN = 0,
        WIFI_LIST,
        WIFI_EDIT,
        SSH_LIST,
        SSH_EDIT,
        TEXT_EDIT,
    };

    enum class EditTarget : uint8_t {
        NONE = 0,
        WIFI_SSID,
        WIFI_PASS,
        SSH_LABEL,
        SSH_HOST,
        SSH_PORT,
        SSH_USER,
        SSH_KEY_PATH,
        SSH_SECRET,
    };

    Screen screen_ = Screen::MAIN;
    Screen edit_return_screen_ = Screen::MAIN;
    int main_sel_ = 0;
    int wifi_slot_ = 0;
    int wifi_edit_sel_ = 0;
    int ssh_slot_ = 0;
    int ssh_edit_sel_ = 0;
    int ssh_edit_scroll_ = 0;
    bool dirty_ = true;

    // Text editor state
    EditTarget edit_target_ = EditTarget::NONE;
    char edit_title_[32] = {0};
    char edit_buf_[MAX_KEY_PATH_LEN] = {0};
    size_t edit_max_len_ = 0;
    size_t edit_len_ = 0;
    size_t edit_cursor_ = 0;
    bool edit_masked_ = false;
    bool edit_numeric_ = false;

    void startTextEdit(Screen return_screen,
                       EditTarget target,
                       const char* title,
                       const char* initial,
                       size_t max_len,
                       bool masked,
                       bool numeric = false);
    void commitTextEdit(AppConfig& cfg, bool& cfg_dirty);

    MenuAction handleMainKey(MenuKey key, char ch);
    void handleWifiListKey(MenuKey key, char ch, AppConfig& cfg, bool& cfg_dirty);
    void handleWifiEditKey(MenuKey key, AppConfig& cfg, bool& cfg_dirty);
    void handleSshListKey(MenuKey key, char ch, AppConfig& cfg, bool& cfg_dirty);
    void handleSshEditKey(MenuKey key, AppConfig& cfg, bool& cfg_dirty);
    void handleTextEditKey(MenuKey key, char ch, AppConfig& cfg, bool& cfg_dirty);

    void render(const AppConfig& cfg);
    void drawHeader(const char* title, uint16_t accent = 0x07FF);
    void drawFooter(const char* hint);
    void drawMenuItem(int row, const char* text, bool selected, bool marked = false);
    void renderMain(const AppConfig& cfg);
    void renderWifiList(const AppConfig& cfg);
    void renderWifiEdit(const AppConfig& cfg);
    void renderSshList(const AppConfig& cfg);
    void renderSshEdit(const AppConfig& cfg);
    void renderTextEdit();
};

}  // namespace esp_cpa_ssh
