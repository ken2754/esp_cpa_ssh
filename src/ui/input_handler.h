#pragma once

#include <M5Cardputer.h>
#include <stddef.h>
#include <stdint.h>

namespace esp_cpa_ssh {

enum class MenuKey : uint8_t {
    NONE = 0,
    UP,
    DOWN,
    LEFT,
    RIGHT,
    ENTER,
    ESC,
    BACKSPACE,
    CHAR,
};

class InputHandler {
public:
    InputHandler() = default;

    // Poll keyboard & BtnA for terminal mode.
    // Returns number of bytes written to out_buf.
    size_t poll(uint8_t* out_buf, size_t max_len, bool& out_menu_requested, int& out_scroll_delta);

    // Poll keyboard & BtnA for MenuUi / dialogs.
    MenuKey pollMenuKey(char& out_char);

    bool fnActive() const { return fn_active_; }
    bool ctrlActive() const { return ctrl_active_; }
    bool altActive() const { return alt_active_; }

private:
    static constexpr uint32_t REPEAT_DELAY_MS = 420;
    static constexpr uint32_t REPEAT_RATE_MS = 55;

    bool fn_active_ = false;
    bool ctrl_active_ = false;
    bool alt_active_ = false;

    bool key_held_ = false;
    uint32_t key_down_ms_ = 0;
    uint32_t last_repeat_ms_ = 0;

    bool shouldEmitKeyEvent();
    size_t translateTerminal(uint8_t* out_buf, size_t max_len, bool& out_menu_requested, int& out_scroll_delta);
    static size_t appendBytes(uint8_t* dst, size_t pos, size_t max_len, const char* src, size_t n);
};

}  // namespace esp_cpa_ssh
