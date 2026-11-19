#include "menu_ui.h"

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esp_cpa_ssh {

namespace {

constexpr uint16_t kBgColor = 0x0000;
constexpr uint16_t kHeaderBg = 0x10A2;
constexpr uint16_t kSelBg = 0x02B5;
constexpr uint16_t kTextFg = 0xFFFF;
constexpr uint16_t kDimFg = 0x8410;
constexpr uint16_t kAccentFg = 0x07FF;
constexpr uint16_t kGoodFg = 0x07E0;
constexpr uint16_t kWarnFg = 0xFFE0;
constexpr uint16_t kErrFg = 0xF800;

constexpr int kMainItemCount = 5;
constexpr int kWifiEditItemCount = 4;
constexpr int kSshEditItemCount = 9;
constexpr int kVisibleMenuRows = 10;  // y = 20..119 (10 rows of 10px)

void copyBounded(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

}  // namespace

void MenuUi::begin() {
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextWrap(false);
    dirty_ = true;
}

void MenuUi::resetToMain() {
    screen_ = Screen::MAIN;
    dirty_ = true;
}

MenuAction MenuUi::update(InputHandler& input, AppConfig& cfg, bool& cfg_dirty) {
    char ch = '\0';
    MenuKey key = input.pollMenuKey(ch);
    MenuAction action = MenuAction::NONE;

    if (key != MenuKey::NONE) {
        switch (screen_) {
            case Screen::MAIN:
                action = handleMainKey(key, ch);
                break;
            case Screen::WIFI_LIST:
                handleWifiListKey(key, ch, cfg, cfg_dirty);
                break;
            case Screen::WIFI_EDIT:
                handleWifiEditKey(key, cfg, cfg_dirty);
                break;
            case Screen::SSH_LIST:
                handleSshListKey(key, ch, cfg, cfg_dirty);
                break;
            case Screen::SSH_EDIT:
                handleSshEditKey(key, cfg, cfg_dirty);
                break;
            case Screen::TEXT_EDIT:
                handleTextEditKey(key, ch, cfg, cfg_dirty);
                break;
        }
        dirty_ = true;
    }

    if (dirty_) {
        render(cfg);
        dirty_ = false;
    }

    return action;
}

MenuAction MenuUi::handleMainKey(MenuKey key, char ch) {
    if (key == MenuKey::UP) {
        main_sel_ = (main_sel_ + kMainItemCount - 1) % kMainItemCount;
        return MenuAction::NONE;
    }
    if (key == MenuKey::DOWN) {
        main_sel_ = (main_sel_ + 1) % kMainItemCount;
        return MenuAction::NONE;
    }
    if (key == MenuKey::CHAR && ch >= '1' && ch <= '5') {
        main_sel_ = ch - '1';
        key = MenuKey::ENTER;
    }
    if (key != MenuKey::ENTER) {
        return MenuAction::NONE;
    }

    switch (main_sel_) {
        case 0:
            return MenuAction::CONNECT_SSH;
        case 1:
            screen_ = Screen::WIFI_LIST;
            return MenuAction::NONE;
        case 2:
            screen_ = Screen::SSH_LIST;
            return MenuAction::NONE;
        case 3:
            return MenuAction::IMPORT_SD;
        case 4:
            return MenuAction::EXIT_LAUNCHER;
        default:
            return MenuAction::NONE;
    }
}

void MenuUi::handleWifiListKey(MenuKey key, char ch, AppConfig& cfg, bool& cfg_dirty) {
    if (key == MenuKey::ESC) {
        screen_ = Screen::MAIN;
        return;
    }
    if (key == MenuKey::UP) {
        wifi_slot_ = (wifi_slot_ + MAX_WIFI_PROFILES - 1) % MAX_WIFI_PROFILES;
        return;
    }
    if (key == MenuKey::DOWN) {
        wifi_slot_ = (wifi_slot_ + 1) % MAX_WIFI_PROFILES;
        return;
    }
    if (key == MenuKey::CHAR && ch >= '1' && ch <= '3') {
        wifi_slot_ = ch - '1';
        key = MenuKey::ENTER;
    }
    if (key == MenuKey::ENTER) {
        wifi_edit_sel_ = 0;
        screen_ = Screen::WIFI_EDIT;
    }
    (void)cfg;
    (void)cfg_dirty;
}

void MenuUi::handleWifiEditKey(MenuKey key, AppConfig& cfg, bool& cfg_dirty) {
    if (key == MenuKey::ESC) {
        screen_ = Screen::WIFI_LIST;
        return;
    }
    if (key == MenuKey::UP) {
        wifi_edit_sel_ = (wifi_edit_sel_ + kWifiEditItemCount - 1) % kWifiEditItemCount;
        return;
    }
    if (key == MenuKey::DOWN) {
        wifi_edit_sel_ = (wifi_edit_sel_ + 1) % kWifiEditItemCount;
        return;
    }
    if (key != MenuKey::ENTER) {
        return;
    }

    WifiProfile& wp = cfg.wifi[wifi_slot_];
    switch (wifi_edit_sel_) {
        case 0:  // Select as active
            cfg.last_wifi_idx = static_cast<int8_t>(wifi_slot_);
            cfg_dirty = true;
            screen_ = Screen::WIFI_LIST;
            break;
        case 1:  // Edit SSID
            startTextEdit(Screen::WIFI_EDIT, EditTarget::WIFI_SSID, "Wi-Fi SSID",
                          wp.ssid, WIFI_SSID_BUF_LEN, false);
            break;
        case 2:  // Edit Password
            startTextEdit(Screen::WIFI_EDIT, EditTarget::WIFI_PASS, "Wi-Fi Password",
                          wp.password, WIFI_PASS_BUF_LEN, true);
            break;
        case 3:  // Delete
            wp = WifiProfile{};
            cfg_dirty = true;
            screen_ = Screen::WIFI_LIST;
            break;
    }
}

void MenuUi::handleSshListKey(MenuKey key, char ch, AppConfig& cfg, bool& cfg_dirty) {
    if (key == MenuKey::ESC) {
        screen_ = Screen::MAIN;
        return;
    }
    if (key == MenuKey::UP) {
        ssh_slot_ = (ssh_slot_ + MAX_SSH_PROFILES - 1) % MAX_SSH_PROFILES;
        return;
    }
    if (key == MenuKey::DOWN) {
        ssh_slot_ = (ssh_slot_ + 1) % MAX_SSH_PROFILES;
        return;
    }
    if (key == MenuKey::CHAR && ch >= '1' && ch <= '3') {
        ssh_slot_ = ch - '1';
        key = MenuKey::ENTER;
    }
    if (key == MenuKey::ENTER) {
        ssh_edit_sel_ = 0;
        ssh_edit_scroll_ = 0;
        screen_ = Screen::SSH_EDIT;
    }
    (void)cfg;
    (void)cfg_dirty;
}

void MenuUi::handleSshEditKey(MenuKey key, AppConfig& cfg, bool& cfg_dirty) {
    if (key == MenuKey::ESC) {
        screen_ = Screen::SSH_LIST;
        return;
    }
    if (key == MenuKey::UP) {
        ssh_edit_sel_ = (ssh_edit_sel_ + kSshEditItemCount - 1) % kSshEditItemCount;
    } else if (key == MenuKey::DOWN) {
        ssh_edit_sel_ = (ssh_edit_sel_ + 1) % kSshEditItemCount;
    } else if (key == MenuKey::ENTER) {
        SshProfile& sp = cfg.ssh[ssh_slot_];
        char port_buf[8];
        switch (ssh_edit_sel_) {
            case 0:  // Use Profile
                cfg.last_ssh_idx = static_cast<int8_t>(ssh_slot_);
                cfg_dirty = true;
                screen_ = Screen::SSH_LIST;
                return;
            case 1:  // Label
                startTextEdit(Screen::SSH_EDIT, EditTarget::SSH_LABEL, "Profile Label",
                              sp.label, MAX_LABEL_LEN, false);
                return;
            case 2:  // Host
                startTextEdit(Screen::SSH_EDIT, EditTarget::SSH_HOST, "SSH Host / IP",
                              sp.host, MAX_HOST_LEN, false);
                return;
            case 3:  // Port
                std::snprintf(port_buf, sizeof(port_buf), "%u", static_cast<unsigned>(sp.port));
                startTextEdit(Screen::SSH_EDIT, EditTarget::SSH_PORT, "SSH Port",
                              port_buf, 6, false, true);
                return;
            case 4:  // Username
                startTextEdit(Screen::SSH_EDIT, EditTarget::SSH_USER, "SSH Username",
                              sp.username, MAX_USER_LEN, false);
                return;
            case 5:  // Auth Type toggle
                sp.auth_type = (sp.auth_type == AUTH_ED25519) ? AUTH_PASSWORD : AUTH_ED25519;
                if (sp.auth_type == AUTH_ED25519 && sp.key_path[0] == '\0') {
                    copyBounded(sp.key_path, MAX_KEY_PATH_LEN, DEFAULT_KEY_PATH);
                }
                cfg_dirty = true;
                return;
            case 6:  // Key Path
                startTextEdit(Screen::SSH_EDIT, EditTarget::SSH_KEY_PATH, "Ed25519 Key Path",
                              sp.key_path[0] ? sp.key_path : DEFAULT_KEY_PATH, MAX_KEY_PATH_LEN, false);
                return;
            case 7:  // Secret
                startTextEdit(Screen::SSH_EDIT, EditTarget::SSH_SECRET,
                              (sp.auth_type == AUTH_ED25519) ? "Key Passphrase" : "SSH Password",
                              sp.secret, MAX_SECRET_LEN, true);
                return;
            case 8:  // Delete
                sp = SshProfile{};
                cfg_dirty = true;
                screen_ = Screen::SSH_LIST;
                return;
        }
    }

    if (ssh_edit_sel_ < ssh_edit_scroll_) {
        ssh_edit_scroll_ = ssh_edit_sel_;
    } else if (ssh_edit_sel_ >= ssh_edit_scroll_ + kVisibleMenuRows) {
        ssh_edit_scroll_ = ssh_edit_sel_ - kVisibleMenuRows + 1;
    }
}

void MenuUi::startTextEdit(Screen return_screen,
                           EditTarget target,
                           const char* title,
                           const char* initial,
                           size_t max_len,
                           bool masked,
                           bool numeric) {
    edit_return_screen_ = return_screen;
    edit_target_ = target;
    copyBounded(edit_title_, sizeof(edit_title_), title);
    edit_max_len_ = std::min(max_len, sizeof(edit_buf_));
    copyBounded(edit_buf_, edit_max_len_, initial);
    edit_len_ = std::strlen(edit_buf_);
    edit_cursor_ = edit_len_;
    edit_masked_ = masked;
    edit_numeric_ = numeric;
    screen_ = Screen::TEXT_EDIT;
}

void MenuUi::commitTextEdit(AppConfig& cfg, bool& cfg_dirty) {
    switch (edit_target_) {
        case EditTarget::WIFI_SSID:
            copyBounded(cfg.wifi[wifi_slot_].ssid, WIFI_SSID_BUF_LEN, edit_buf_);
            cfg.wifi[wifi_slot_].configured = (cfg.wifi[wifi_slot_].ssid[0] != '\0');
            break;
        case EditTarget::WIFI_PASS:
            copyBounded(cfg.wifi[wifi_slot_].password, WIFI_PASS_BUF_LEN, edit_buf_);
            break;
        case EditTarget::SSH_LABEL:
            copyBounded(cfg.ssh[ssh_slot_].label, MAX_LABEL_LEN, edit_buf_);
            break;
        case EditTarget::SSH_HOST:
            copyBounded(cfg.ssh[ssh_slot_].host, MAX_HOST_LEN, edit_buf_);
            cfg.ssh[ssh_slot_].configured =
                (cfg.ssh[ssh_slot_].host[0] != '\0' && cfg.ssh[ssh_slot_].username[0] != '\0');
            break;
        case EditTarget::SSH_PORT: {
            long p = std::strtol(edit_buf_, nullptr, 10);
            if (p < 1 || p > 65535) p = 22;
            cfg.ssh[ssh_slot_].port = static_cast<uint16_t>(p);
            break;
        }
        case EditTarget::SSH_USER:
            copyBounded(cfg.ssh[ssh_slot_].username, MAX_USER_LEN, edit_buf_);
            cfg.ssh[ssh_slot_].configured =
                (cfg.ssh[ssh_slot_].host[0] != '\0' && cfg.ssh[ssh_slot_].username[0] != '\0');
            break;
        case EditTarget::SSH_KEY_PATH:
            copyBounded(cfg.ssh[ssh_slot_].key_path, MAX_KEY_PATH_LEN, edit_buf_);
            break;
        case EditTarget::SSH_SECRET:
            copyBounded(cfg.ssh[ssh_slot_].secret, MAX_SECRET_LEN, edit_buf_);
            break;
        case EditTarget::NONE:
            return;
    }
    cfg_dirty = true;
}

void MenuUi::handleTextEditKey(MenuKey key, char ch, AppConfig& cfg, bool& cfg_dirty) {
    if (key == MenuKey::ESC) {
        screen_ = edit_return_screen_;
        return;
    }
    if (key == MenuKey::ENTER) {
        commitTextEdit(cfg, cfg_dirty);
        screen_ = edit_return_screen_;
        return;
    }
    if (key == MenuKey::LEFT) {
        if (edit_cursor_ > 0) edit_cursor_--;
        return;
    }
    if (key == MenuKey::RIGHT) {
        if (edit_cursor_ < edit_len_) edit_cursor_++;
        return;
    }
    if (key == MenuKey::BACKSPACE) {
        if (edit_cursor_ > 0 && edit_len_ > 0) {
            std::memmove(&edit_buf_[edit_cursor_ - 1], &edit_buf_[edit_cursor_], edit_len_ - edit_cursor_ + 1);
            edit_cursor_--;
            edit_len_--;
        }
        return;
    }
    if (key == MenuKey::CHAR && ch >= 0x20 && ch <= 0x7E) {
        if (edit_numeric_ && (ch < '0' || ch > '9')) {
            return;
        }
        if (edit_len_ + 1 < edit_max_len_) {
            std::memmove(&edit_buf_[edit_cursor_ + 1], &edit_buf_[edit_cursor_], edit_len_ - edit_cursor_ + 1);
            edit_buf_[edit_cursor_] = ch;
            edit_cursor_++;
            edit_len_++;
        }
    }
}

void MenuUi::render(const AppConfig& cfg) {
    M5Cardputer.Display.startWrite();
    M5Cardputer.Display.fillScreen(kBgColor);

    switch (screen_) {
        case Screen::MAIN:
            renderMain(cfg);
            break;
        case Screen::WIFI_LIST:
            renderWifiList(cfg);
            break;
        case Screen::WIFI_EDIT:
            renderWifiEdit(cfg);
            break;
        case Screen::SSH_LIST:
            renderSshList(cfg);
            break;
        case Screen::SSH_EDIT:
            renderSshEdit(cfg);
            break;
        case Screen::TEXT_EDIT:
            renderTextEdit();
            break;
    }

    M5Cardputer.Display.endWrite();
}

void MenuUi::drawHeader(const char* title, uint16_t accent) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_WIDTH, 16, kHeaderBg);
    d.drawFastHLine(0, 16, SCREEN_WIDTH, accent);
    d.setTextColor(accent, kHeaderBg);
    d.drawString(title, 4, 4);
}

void MenuUi::drawFooter(const char* hint) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, STATUS_BAR_Y, SCREEN_WIDTH, STATUS_BAR_HEIGHT, kHeaderBg);
    d.drawFastHLine(0, STATUS_BAR_Y, SCREEN_WIDTH, kDimFg);
    d.setTextColor(kDimFg, kHeaderBg);
    d.drawString(hint, 4, STATUS_BAR_Y + 4);
}

void MenuUi::drawMenuItem(int row, const char* text, bool selected, bool marked) {
    auto& d = M5Cardputer.Display;
    int y = 20 + row * 10;
    uint16_t bg = selected ? kSelBg : kBgColor;
    uint16_t fg = selected ? kTextFg : (marked ? kGoodFg : kTextFg);
    if (selected) {
        d.fillRect(0, y, SCREEN_WIDTH, 10, bg);
    }
    d.setTextColor(fg, bg);
    char line[41];
    std::snprintf(line, sizeof(line), "%c%s", selected ? '>' : ' ', text);
    d.drawString(line, 2, y + 1);
}

void MenuUi::renderMain(const AppConfig& cfg) {
    drawHeader("ESP-CPA-SSH  Cardputer ADV");

    const char* items[kMainItemCount] = {
        "1. Connect SSH",
        "2. Wi-Fi Profiles",
        "3. SSH Profiles",
        "4. Import from SD",
        "5. Exit to M5Launcher",
    };
    for (int i = 0; i < kMainItemCount; ++i) {
        drawMenuItem(i, items[i], main_sel_ == i);
    }

    // Active profile summary
    auto& d = M5Cardputer.Display;
    d.setTextColor(kDimFg, kBgColor);
    int w_idx = (cfg.last_wifi_idx >= 0 && cfg.last_wifi_idx < static_cast<int>(MAX_WIFI_PROFILES))
                    ? cfg.last_wifi_idx
                    : 0;
    int s_idx = (cfg.last_ssh_idx >= 0 && cfg.last_ssh_idx < static_cast<int>(MAX_SSH_PROFILES))
                    ? cfg.last_ssh_idx
                    : 0;

    char summary[41];
    std::snprintf(summary, sizeof(summary), "WiFi: %.32s",
                  cfg.wifi[w_idx].configured ? cfg.wifi[w_idx].ssid : "<none>");
    d.drawString(summary, 4, 84);

    const SshProfile& sp = cfg.ssh[s_idx];
    if (sp.configured) {
        std::snprintf(summary, sizeof(summary), "SSH : %s@%s", sp.username, sp.host);
    } else {
        std::snprintf(summary, sizeof(summary), "SSH : <none>");
    }
    d.drawString(summary, 4, 96);

    drawFooter("Fn+;/Fn+. Move  Enter:Select  1-5:Go");
}

void MenuUi::renderWifiList(const AppConfig& cfg) {
    drawHeader("Wi-Fi Profiles");
    for (int i = 0; i < static_cast<int>(MAX_WIFI_PROFILES); ++i) {
        char line[40];
        bool active = (cfg.last_wifi_idx == i);
        std::snprintf(line, sizeof(line), "%d. %c %.30s", i + 1, active ? '*' : ' ',
                      cfg.wifi[i].configured ? cfg.wifi[i].ssid : "<Empty>");
        drawMenuItem(i, line, wifi_slot_ == i, active);
    }
    drawFooter("Enter:Edit Slot  Esc/BtnA:Back");
}

void MenuUi::renderWifiEdit(const AppConfig& cfg) {
    char title[32];
    std::snprintf(title, sizeof(title), "Wi-Fi Slot %d", wifi_slot_ + 1);
    drawHeader(title);

    const WifiProfile& wp = cfg.wifi[wifi_slot_];
    char items[kWifiEditItemCount][40];
    std::snprintf(items[0], sizeof(items[0]), "Set Active%s",
                  (cfg.last_wifi_idx == wifi_slot_) ? " [ACTIVE]" : "");
    std::snprintf(items[1], sizeof(items[1]), "SSID: %.30s", wp.ssid[0] ? wp.ssid : "<empty>");
    std::snprintf(items[2], sizeof(items[2]), "Pass: %s", wp.password[0] ? "********" : "<empty>");
    std::snprintf(items[3], sizeof(items[3]), "Delete Slot");

    for (int i = 0; i < kWifiEditItemCount; ++i) {
        drawMenuItem(i, items[i], wifi_edit_sel_ == i);
    }
    drawFooter("Enter:Edit/Select  Esc/BtnA:Back");
}

void MenuUi::renderSshList(const AppConfig& cfg) {
    drawHeader("SSH Profiles");
    for (int i = 0; i < static_cast<int>(MAX_SSH_PROFILES); ++i) {
        char line[40];
        bool active = (cfg.last_ssh_idx == i);
        const SshProfile& sp = cfg.ssh[i];
        if (sp.configured) {
            const char* name = sp.label[0] ? sp.label : sp.host;
            std::snprintf(line, sizeof(line), "%d. %c %.30s", i + 1, active ? '*' : ' ', name);
        } else {
            std::snprintf(line, sizeof(line), "%d. %c <Empty>", i + 1, active ? '*' : ' ');
        }
        drawMenuItem(i, line, ssh_slot_ == i, active);
    }
    drawFooter("Enter:Edit Slot  Esc/BtnA:Back");
}

void MenuUi::renderSshEdit(const AppConfig& cfg) {
    char title[32];
    std::snprintf(title, sizeof(title), "SSH Slot %d", ssh_slot_ + 1);
    drawHeader(title);

    const SshProfile& sp = cfg.ssh[ssh_slot_];
    char items[kSshEditItemCount][40];
    std::snprintf(items[0], sizeof(items[0]), "Use Profile%s",
                  (cfg.last_ssh_idx == ssh_slot_) ? " [ACTIVE]" : "");
    std::snprintf(items[1], sizeof(items[1]), "Label: %.28s", sp.label[0] ? sp.label : "<none>");
    std::snprintf(items[2], sizeof(items[2]), "Host : %.28s", sp.host[0] ? sp.host : "<empty>");
    std::snprintf(items[3], sizeof(items[3]), "Port : %u", static_cast<unsigned>(sp.port));
    std::snprintf(items[4], sizeof(items[4]), "User : %.28s", sp.username[0] ? sp.username : "<empty>");
    std::snprintf(items[5], sizeof(items[5]), "Auth : %s",
                  (sp.auth_type == AUTH_ED25519) ? "Ed25519 Key" : "Password");
    std::snprintf(items[6], sizeof(items[6]), "Key  : %.28s",
                  sp.key_path[0] ? sp.key_path : DEFAULT_KEY_PATH);
    std::snprintf(items[7], sizeof(items[7]), "%s: %s",
                  (sp.auth_type == AUTH_ED25519) ? "Passp" : "Pass ",
                  sp.secret[0] ? "********" : "<empty>");
    std::snprintf(items[8], sizeof(items[8]), "Delete Slot");

    for (int row = 0; row < kVisibleMenuRows; ++row) {
        int idx = ssh_edit_scroll_ + row;
        if (idx >= kSshEditItemCount) break;
        drawMenuItem(row, items[idx], ssh_edit_sel_ == idx);
    }
    drawFooter("Enter:Edit/Toggle  Esc/BtnA:Back");
}

void MenuUi::renderTextEdit() {
    drawHeader(edit_title_, kWarnFg);

    auto& d = M5Cardputer.Display;
    d.drawRect(4, 26, SCREEN_WIDTH - 8, 56, kAccentFg);

    constexpr int kColsPerLine = 37;
    int y = 32;
    for (int line = 0; line < 4; ++line) {
        size_t start = static_cast<size_t>(line * kColsPerLine);
        if (start > edit_len_) break;

        char row_buf[kColsPerLine + 1];
        size_t count = std::min(static_cast<size_t>(kColsPerLine), edit_len_ - start);
        for (size_t i = 0; i < count; ++i) {
            row_buf[i] = edit_masked_ ? '*' : edit_buf_[start + i];
        }
        row_buf[count] = '\0';
        d.setTextColor(kTextFg, kBgColor);
        d.drawString(row_buf, 8, y + line * 11);
    }

    // Draw cursor underline
    int cur_line = static_cast<int>(edit_cursor_ / kColsPerLine);
    int cur_col = static_cast<int>(edit_cursor_ % kColsPerLine);
    if (cur_line < 4) {
        int cx = 8 + cur_col * FONT_WIDTH;
        int cy = y + cur_line * 11 + FONT_HEIGHT;
        d.drawFastHLine(cx, cy, FONT_WIDTH, kWarnFg);
    }

    char len_info[32];
    std::snprintf(len_info, sizeof(len_info), "Len: %u/%u",
                  static_cast<unsigned>(edit_len_),
                  static_cast<unsigned>(edit_max_len_ > 0 ? edit_max_len_ - 1 : 0));
    d.setTextColor(kDimFg, kBgColor);
    d.drawString(len_info, 6, 90);

    drawFooter("Enter:Save  Del:Erase  Esc:Cancel");
}

bool MenuUi::showHostVerifyDialog(InputHandler& input,
                                  const char* host,
                                  uint16_t port,
                                  const char* key_type,
                                  const char* sha256_fp,
                                  bool is_mismatch) {
    bool accept_sel = !is_mismatch;
    bool redraw = true;

    while (true) {
        M5Cardputer.update();

        char ch = '\0';
        MenuKey key = input.pollMenuKey(ch);
        if (key == MenuKey::LEFT || key == MenuKey::RIGHT || key == MenuKey::UP || key == MenuKey::DOWN) {
            accept_sel = !accept_sel;
            redraw = true;
        } else if (key == MenuKey::ENTER) {
            return accept_sel;
        } else if (key == MenuKey::ESC) {
            return false;
        } else if (key == MenuKey::CHAR) {
            if (ch == 'y' || ch == 'Y' || ch == 'a' || ch == 'A') return true;
            if (ch == 'n' || ch == 'N' || ch == 'r' || ch == 'R') return false;
        }

        if (redraw) {
            auto& d = M5Cardputer.Display;
            d.startWrite();
            d.fillScreen(kBgColor);
            drawHeader(is_mismatch ? "!! HOST KEY CHANGED !!" : "Verify SSH Host Key",
                       is_mismatch ? kErrFg : kWarnFg);

            char line[41];
            d.setTextColor(kTextFg, kBgColor);
            std::snprintf(line, sizeof(line), "Host: %.26s:%u", host ? host : "?", static_cast<unsigned>(port));
            d.drawString(line, 4, 22);

            std::snprintf(line, sizeof(line), "Type: %.32s", key_type ? key_type : "unknown");
            d.drawString(line, 4, 34);

            d.setTextColor(kAccentFg, kBgColor);
            d.drawString("SHA256 Fingerprint:", 4, 48);

            d.setTextColor(kTextFg, kBgColor);
            const char* fp = sha256_fp ? sha256_fp : "";
            size_t fp_len = std::strlen(fp);
            for (int row = 0; row < 2; ++row) {
                size_t off = static_cast<size_t>(row * 38);
                if (off >= fp_len) break;
                std::snprintf(line, sizeof(line), "%.38s", fp + off);
                d.drawString(line, 4, 58 + row * 10);
            }

            if (is_mismatch) {
                d.setTextColor(kErrFg, kBgColor);
                d.drawString("WARNING: Possible MITM attack!", 4, 82);
            }

            // Accept / Reject buttons
            d.fillRect(28, 96, 80, 16, accept_sel ? kGoodFg : kHeaderBg);
            d.setTextColor(accept_sel ? kBgColor : kTextFg, accept_sel ? kGoodFg : kHeaderBg);
            d.drawString("Accept (Y)", 38, 100);

            d.fillRect(132, 96, 80, 16, !accept_sel ? kErrFg : kHeaderBg);
            d.setTextColor(!accept_sel ? kBgColor : kTextFg, !accept_sel ? kErrFg : kHeaderBg);
            d.drawString("Reject (N)", 142, 100);

            drawFooter("Left/Right + Enter  or  Y / N");
            d.endWrite();
            redraw = false;
        }

        delay(15);
    }
}

void drawWrappedText(const char* text, int x, int& y, int max_cols, int max_y, int line_height, uint16_t fg) {
    if (!text || text[0] == '\0') return;
    auto& d = M5Cardputer.Display;
    d.setTextColor(fg, kBgColor);

    const char* p = text;
    char buf[41];
    while (*p != '\0' && y <= max_y) {
        int len = 0;
        int last_space = -1;
        bool hit_newline = false;
        while (p[len] != '\0' && len < max_cols) {
            if (p[len] == '\n') {
                hit_newline = true;
                break;
            }
            if (p[len] == ' ') {
                last_space = len;
            }
            ++len;
        }

        int draw_len = len;
        if (!hit_newline && p[len] != '\0' && last_space > 0) {
            draw_len = last_space;
        }

        int copy_len = std::min(draw_len, 40);
        std::memcpy(buf, p, copy_len);
        buf[copy_len] = '\0';
        d.drawString(buf, x, y);
        y += line_height;

        p += draw_len;
        if (hit_newline && *p == '\n') {
            ++p;
        } else {
            while (*p == ' ') {
                ++p;
            }
        }
    }
}

void MenuUi::showStatusScreen(const char* title, const char* line1, const char* line2, uint16_t accent) {
    auto& d = M5Cardputer.Display;
    d.startWrite();
    d.fillScreen(kBgColor);
    drawHeader(title ? title : "Status", accent);
    int y = 24;
    if (line1) {
        drawWrappedText(line1, 4, y, 38, STATUS_BAR_Y - 10, 10, kTextFg);
    }
    if (line2 && line2[0] != '\0') {
        y += 4;
        drawWrappedText(line2, 4, y, 38, STATUS_BAR_Y - 10, 10, kDimFg);
    }
    d.endWrite();
}

void MenuUi::showMessageAndWait(InputHandler& input,
                                const char* title,
                                const char* line1,
                                const char* line2,
                                uint16_t accent) {
    showStatusScreen(title, line1, line2, accent);
    drawFooter("Press any key or BtnA to continue");

    while (true) {
        M5Cardputer.update();
        char ch = '\0';
        if (input.pollMenuKey(ch) != MenuKey::NONE) {
            dirty_ = true;
            return;
        }
        delay(15);
    }
}

}  // namespace esp_cpa_ssh
