#pragma once

#include "app_types.h"
#include <M5Cardputer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <stddef.h>
#include <stdint.h>

namespace esp_cpa_ssh {

class TerminalView : public ITerminalView {
public:
    static constexpr int SCROLLBACK_LINES = 64;
    static constexpr size_t RX_RING_SIZE = 4096;

    TerminalView();
    ~TerminalView() override;

    void begin() override;
    void clear() override;
    void feed(const uint8_t* data, size_t len) override;
    void renderIfDirty(const StatusInfo& status) override;
    void scrollUp(int lines) override;
    void scrollDown(int lines) override;

    int scrollOffset() const { return scroll_offset_; }

private:
    struct Cell {
        char ch;
        uint8_t fg;     // 0..15 ANSI color index
        uint8_t bg;     // 0..15 ANSI color index
        uint8_t attrs;  // bit 0: bold, bit 1: reverse
    };

    enum class ParseState : uint8_t {
        NORMAL = 0,
        ESC,
        ESC_SKIP_ONE,
        CSI,
        OSC,
        OSC_ESC,
    };

    // Display sprite (8-bit, 240x135 = 32.4 KB)
    M5Canvas canvas_;
    bool canvas_ready_ = false;
    bool initialized_ = false;

    // Terminal grid & scrollback
    Cell screen_[TERM_ROWS][TERM_COLS];
    Cell scrollback_[SCROLLBACK_LINES][TERM_COLS];
    int sb_head_ = 0;   // Next write row in scrollback_
    int sb_count_ = 0;  // Number of valid lines in scrollback_ (0..SCROLLBACK_LINES)
    int scroll_offset_ = 0;

    // Cursor & attributes
    int cur_row_ = 0;
    int cur_col_ = 0;
    int saved_row_ = 0;
    int saved_col_ = 0;
    uint8_t cur_fg_ = 7;
    uint8_t cur_bg_ = 0;
    uint8_t cur_attrs_ = 0;
    bool cursor_visible_ = true;
    bool cursor_blink_on_ = true;
    uint32_t last_blink_ms_ = 0;

    // Thread-safe RX ring buffer
    uint8_t rx_ring_[RX_RING_SIZE];
    volatile size_t rx_head_ = 0;
    volatile size_t rx_tail_ = 0;
    portMUX_TYPE rx_mux_ = portMUX_INITIALIZER_UNLOCKED;

    // VT100/ANSI parser state
    ParseState parse_state_ = ParseState::NORMAL;
    static constexpr int MAX_CSI_PARAMS = 8;
    int csi_params_[MAX_CSI_PARAMS] = {0};
    int csi_param_count_ = 0;
    int csi_cur_param_ = 0;
    bool csi_has_digit_ = false;
    bool csi_private_ = false;

    // UTF-8 decoder state
    uint8_t utf8_remaining_ = 0;
    uint32_t utf8_codepoint_ = 0;

    // Dirty tracking
    bool dirty_ = true;
    StatusInfo last_status_{};
    bool has_last_status_ = false;

    void drainRxRing();
    void processByte(uint8_t b);
    void handleNormalByte(uint8_t b);
    void handleEscByte(uint8_t b);
    void handleCsiByte(uint8_t b);
    void executeCsi(char final_char);
    void executeSgr();

    void putChar(char ch);
    void lineFeed();
    void reverseIndex();
    void scrollRegionUp();
    void scrollRegionDown();
    void clearRow(int row, int from_col = 0, int to_col = TERM_COLS - 1);
    void resetAttributes();
    void clampCursor();

    uint8_t map256Color(int idx) const;
    uint16_t paletteColor(uint8_t idx) const;
    void drawTerminalGrid();
    void drawStatusBar(const StatusInfo& status);
};

}  // namespace esp_cpa_ssh
