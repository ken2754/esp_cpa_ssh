#include "terminal_view.h"

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace esp_cpa_ssh {

namespace {

constexpr uint8_t ATTR_BOLD = 0x01;
constexpr uint8_t ATTR_REVERSE = 0x02;
constexpr uint32_t CURSOR_BLINK_MS = 500;

// Standard 16-color ANSI palette in RGB565
constexpr uint16_t kAnsiPalette[16] = {
    0x0000,  // 0: Black
    0xA800,  // 1: Red
    0x0540,  // 2: Green
    0xAA80,  // 3: Yellow / Brown
    0x0015,  // 4: Blue
    0xA815,  // 5: Magenta
    0x0555,  // 6: Cyan
    0xBDF7,  // 7: Light Gray
    0x52AA,  // 8: Dark Gray
    0xF800,  // 9: Bright Red
    0x07E0,  // 10: Bright Green
    0xFFE0,  // 11: Bright Yellow
    0x555F,  // 12: Bright Blue
    0xF81F,  // 13: Bright Magenta
    0x07FF,  // 14: Bright Cyan
    0xFFFF,  // 15: White
};

}  // namespace

TerminalView::TerminalView() : canvas_(&M5Cardputer.Display) {
    clear();
}

TerminalView::~TerminalView() {
    if (canvas_ready_) {
        canvas_.deleteSprite();
        canvas_ready_ = false;
    }
}

void TerminalView::begin() {
    if (!initialized_) {
        canvas_.setColorDepth(8);
        canvas_ready_ = (canvas_.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT) != nullptr);
        if (canvas_ready_) {
            canvas_.setFont(&fonts::Font0);
            canvas_.setTextSize(1);
            canvas_.setTextWrap(false);
        } else {
            M5Cardputer.Display.setFont(&fonts::Font0);
            M5Cardputer.Display.setTextSize(1);
            M5Cardputer.Display.setTextWrap(false);
        }
        initialized_ = true;
    }
    clear();
    last_blink_ms_ = millis();
    dirty_ = true;
}

void TerminalView::clear() {
    resetAttributes();
    for (int r = 0; r < TERM_ROWS; ++r) {
        clearRow(r);
    }
    cur_row_ = 0;
    cur_col_ = 0;
    saved_row_ = 0;
    saved_col_ = 0;
    scroll_offset_ = 0;
    dirty_ = true;
}

void TerminalView::feed(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    portENTER_CRITICAL(&rx_mux_);
    for (size_t i = 0; i < len; ++i) {
        size_t next_head = (rx_head_ + 1) % RX_RING_SIZE;
        if (next_head == rx_tail_) {
            // Ring full: advance tail to drop oldest byte rather than blocking SSH task
            rx_tail_ = (rx_tail_ + 1) % RX_RING_SIZE;
        }
        rx_ring_[rx_head_] = data[i];
        rx_head_ = next_head;
    }
    portEXIT_CRITICAL(&rx_mux_);
}

void TerminalView::scrollUp(int lines) {
    if (lines <= 0) return;
    int next = scroll_offset_ + lines;
    if (next > sb_count_) next = sb_count_;
    if (next != scroll_offset_) {
        scroll_offset_ = next;
        dirty_ = true;
    }
}

void TerminalView::scrollDown(int lines) {
    if (lines <= 0) return;
    int next = scroll_offset_ - lines;
    if (next < 0) next = 0;
    if (next != scroll_offset_) {
        scroll_offset_ = next;
        dirty_ = true;
    }
}

void TerminalView::renderIfDirty(const StatusInfo& status) {
    if (!initialized_) {
        begin();
    }

    drainRxRing();

    uint32_t now = millis();
    if (now - last_blink_ms_ >= CURSOR_BLINK_MS) {
        last_blink_ms_ = now;
        cursor_blink_on_ = !cursor_blink_on_;
        if (cursor_visible_ && scroll_offset_ == 0) {
            dirty_ = true;
        }
    }

    if (!has_last_status_ || std::memcmp(&last_status_, &status, sizeof(StatusInfo)) != 0) {
        last_status_ = status;
        has_last_status_ = true;
        dirty_ = true;
    }

    if (!dirty_) {
        return;
    }

    drawTerminalGrid();
    drawStatusBar(status);

    if (canvas_ready_) {
        canvas_.pushSprite(0, 0);
    }
    dirty_ = false;
}

void TerminalView::drainRxRing() {
    uint8_t local_buf[256];
    while (true) {
        size_t count = 0;
        portENTER_CRITICAL(&rx_mux_);
        while (rx_tail_ != rx_head_ && count < sizeof(local_buf)) {
            local_buf[count++] = rx_ring_[rx_tail_];
            rx_tail_ = (rx_tail_ + 1) % RX_RING_SIZE;
        }
        portEXIT_CRITICAL(&rx_mux_);

        if (count == 0) break;

        for (size_t i = 0; i < count; ++i) {
            processByte(local_buf[i]);
        }
        dirty_ = true;
    }
}

void TerminalView::processByte(uint8_t b) {
    switch (parse_state_) {
        case ParseState::NORMAL:
            handleNormalByte(b);
            break;
        case ParseState::ESC:
            handleEscByte(b);
            break;
        case ParseState::ESC_SKIP_ONE:
            parse_state_ = ParseState::NORMAL;
            break;
        case ParseState::CSI:
            handleCsiByte(b);
            break;
        case ParseState::OSC:
            if (b == 0x07) {
                parse_state_ = ParseState::NORMAL;
            } else if (b == 0x1B) {
                parse_state_ = ParseState::OSC_ESC;
            }
            break;
        case ParseState::OSC_ESC:
            if (b == '\\') {
                parse_state_ = ParseState::NORMAL;
            } else if (b != 0x1B) {
                parse_state_ = ParseState::OSC;
            }
            break;
    }
}

void TerminalView::handleNormalByte(uint8_t b) {
    // Handle ongoing UTF-8 multi-byte sequence
    if (utf8_remaining_ > 0) {
        if ((b & 0xC0) == 0x80) {
            utf8_codepoint_ = (utf8_codepoint_ << 6) | (b & 0x3F);
            utf8_remaining_--;
            if (utf8_remaining_ == 0) {
                char fallback = '?';
                // Map common box-drawing / typographic UTF-8 codepoints to ASCII
                if (utf8_codepoint_ == 0x00A0) {
                    fallback = ' ';
                } else if (utf8_codepoint_ == 0x2018 || utf8_codepoint_ == 0x2019) {
                    fallback = '\'';
                } else if (utf8_codepoint_ == 0x201C || utf8_codepoint_ == 0x201D) {
                    fallback = '"';
                } else if (utf8_codepoint_ == 0x2013 || utf8_codepoint_ == 0x2014 ||
                           utf8_codepoint_ == 0x2500 || utf8_codepoint_ == 0x2501) {
                    fallback = '-';
                } else if (utf8_codepoint_ == 0x2502 || utf8_codepoint_ == 0x2503) {
                    fallback = '|';
                } else if (utf8_codepoint_ >= 0x250C && utf8_codepoint_ <= 0x254B) {
                    fallback = '+';
                } else if (utf8_codepoint_ == 0x2022) {
                    fallback = '*';
                }
                putChar(fallback);
            }
            return;
        }
        // Invalid continuation byte: reset UTF-8 state and process this byte normally
        utf8_remaining_ = 0;
    }

    if (b < 0x20) {
        switch (b) {
            case '\r':
                cur_col_ = 0;
                break;
            case '\n':
            case 0x0B:
            case 0x0C:
                lineFeed();
                break;
            case '\b':
                if (cur_col_ > 0) cur_col_--;
                break;
            case '\t': {
                int next_tab = (cur_col_ + 8) & ~7;
                cur_col_ = (next_tab >= TERM_COLS) ? (TERM_COLS - 1) : next_tab;
                break;
            }
            case 0x1B:
                parse_state_ = ParseState::ESC;
                break;
            default:
                break;
        }
        return;
    }

    if (b >= 0x20 && b <= 0x7E) {
        putChar(static_cast<char>(b));
        return;
    }

    if (b == 0x7F) {
        return;
    }

    // Start of UTF-8 sequence
    if ((b & 0xE0) == 0xC0) {
        utf8_codepoint_ = b & 0x1F;
        utf8_remaining_ = 1;
    } else if ((b & 0xF0) == 0xE0) {
        utf8_codepoint_ = b & 0x0F;
        utf8_remaining_ = 2;
    } else if ((b & 0xF8) == 0xF0) {
        utf8_codepoint_ = b & 0x07;
        utf8_remaining_ = 3;
    }
}

void TerminalView::handleEscByte(uint8_t b) {
    switch (b) {
        case '[':
            parse_state_ = ParseState::CSI;
            csi_param_count_ = 0;
            csi_cur_param_ = 0;
            csi_has_digit_ = false;
            csi_private_ = false;
            std::memset(csi_params_, 0, sizeof(csi_params_));
            break;
        case ']':
            parse_state_ = ParseState::OSC;
            break;
        case '(':
        case ')':
        case '*':
        case '+':
            parse_state_ = ParseState::ESC_SKIP_ONE;
            break;
        case 'D':
            lineFeed();
            parse_state_ = ParseState::NORMAL;
            break;
        case 'M':
            reverseIndex();
            parse_state_ = ParseState::NORMAL;
            break;
        case 'E':
            cur_col_ = 0;
            lineFeed();
            parse_state_ = ParseState::NORMAL;
            break;
        case '7':
            saved_row_ = cur_row_;
            saved_col_ = cur_col_;
            parse_state_ = ParseState::NORMAL;
            break;
        case '8':
            cur_row_ = saved_row_;
            cur_col_ = saved_col_;
            clampCursor();
            parse_state_ = ParseState::NORMAL;
            break;
        case 'c':
            clear();
            parse_state_ = ParseState::NORMAL;
            break;
        default:
            parse_state_ = ParseState::NORMAL;
            break;
    }
}

void TerminalView::handleCsiByte(uint8_t b) {
    if (b >= '0' && b <= '9') {
        csi_cur_param_ = csi_cur_param_ * 10 + (b - '0');
        if (csi_cur_param_ > 9999) csi_cur_param_ = 9999;
        csi_has_digit_ = true;
        return;
    }

    if (b == ';') {
        if (csi_param_count_ < MAX_CSI_PARAMS) {
            csi_params_[csi_param_count_++] = csi_has_digit_ ? csi_cur_param_ : 0;
        }
        csi_cur_param_ = 0;
        csi_has_digit_ = false;
        return;
    }

    if (b == '?' || b == '>' || b == '!') {
        csi_private_ = true;
        return;
    }

    if (b >= 0x20 && b <= 0x2F) {
        // Intermediate bytes: ignore
        return;
    }

    if (b >= 0x40 && b <= 0x7E) {
        if (csi_has_digit_ || csi_param_count_ > 0) {
            if (csi_param_count_ < MAX_CSI_PARAMS) {
                csi_params_[csi_param_count_++] = csi_has_digit_ ? csi_cur_param_ : 0;
            }
        }
        executeCsi(static_cast<char>(b));
        parse_state_ = ParseState::NORMAL;
        return;
    }

    if (b == 0x1B) {
        parse_state_ = ParseState::ESC;
        return;
    }

    parse_state_ = ParseState::NORMAL;
}

void TerminalView::executeCsi(char final_char) {
    auto param = [&](int idx, int def_val) -> int {
        if (idx >= csi_param_count_ || csi_params_[idx] == 0) return def_val;
        return csi_params_[idx];
    };

    if (csi_private_) {
        if (final_char == 'h' || final_char == 'l') {
            bool set_mode = (final_char == 'h');
            for (int i = 0; i < csi_param_count_; ++i) {
                int mode = csi_params_[i];
                if (mode == 25) {
                    cursor_visible_ = set_mode;
                } else if (mode == 1049 || mode == 47 || mode == 1047) {
                    for (int r = 0; r < TERM_ROWS; ++r) {
                        clearRow(r);
                    }
                    cur_row_ = 0;
                    cur_col_ = 0;
                }
            }
        }
        return;
    }

    switch (final_char) {
        case 'A':  // CUU
            cur_row_ -= param(0, 1);
            clampCursor();
            break;
        case 'B':  // CUD
            cur_row_ += param(0, 1);
            clampCursor();
            break;
        case 'C':  // CUF
            cur_col_ += param(0, 1);
            clampCursor();
            break;
        case 'D':  // CUB
            cur_col_ -= param(0, 1);
            clampCursor();
            break;
        case 'E':  // CNL
            cur_row_ += param(0, 1);
            cur_col_ = 0;
            clampCursor();
            break;
        case 'F':  // CPL
            cur_row_ -= param(0, 1);
            cur_col_ = 0;
            clampCursor();
            break;
        case 'G':  // CHA
            cur_col_ = param(0, 1) - 1;
            clampCursor();
            break;
        case 'd':  // VPA
            cur_row_ = param(0, 1) - 1;
            clampCursor();
            break;
        case 'H':  // CUP
        case 'f':  // HVP
            cur_row_ = param(0, 1) - 1;
            cur_col_ = param(1, 1) - 1;
            clampCursor();
            break;
        case 'J': {  // ED
            int mode = (csi_param_count_ > 0) ? csi_params_[0] : 0;
            if (mode == 0) {
                clearRow(cur_row_, cur_col_, TERM_COLS - 1);
                for (int r = cur_row_ + 1; r < TERM_ROWS; ++r) clearRow(r);
            } else if (mode == 1) {
                for (int r = 0; r < cur_row_; ++r) clearRow(r);
                clearRow(cur_row_, 0, cur_col_);
            } else if (mode == 2 || mode == 3) {
                for (int r = 0; r < TERM_ROWS; ++r) clearRow(r);
            }
            break;
        }
        case 'K': {  // EL
            int mode = (csi_param_count_ > 0) ? csi_params_[0] : 0;
            if (mode == 0) {
                clearRow(cur_row_, cur_col_, TERM_COLS - 1);
            } else if (mode == 1) {
                clearRow(cur_row_, 0, cur_col_);
            } else if (mode == 2) {
                clearRow(cur_row_);
            }
            break;
        }
        case 'L': {  // IL - insert lines at cursor row
            int n = std::min(param(0, 1), TERM_ROWS - cur_row_);
            for (int r = TERM_ROWS - 1; r >= cur_row_ + n; --r) {
                std::memcpy(screen_[r], screen_[r - n], sizeof(screen_[r]));
            }
            for (int r = cur_row_; r < cur_row_ + n; ++r) {
                clearRow(r);
            }
            break;
        }
        case 'M': {  // DL - delete lines at cursor row
            int n = std::min(param(0, 1), TERM_ROWS - cur_row_);
            for (int r = cur_row_; r < TERM_ROWS - n; ++r) {
                std::memcpy(screen_[r], screen_[r + n], sizeof(screen_[r]));
            }
            for (int r = TERM_ROWS - n; r < TERM_ROWS; ++r) {
                clearRow(r);
            }
            break;
        }
        case 'P': {  // DCH - delete characters
            int n = std::min(param(0, 1), TERM_COLS - cur_col_);
            for (int c = cur_col_; c < TERM_COLS - n; ++c) {
                screen_[cur_row_][c] = screen_[cur_row_][c + n];
            }
            clearRow(cur_row_, TERM_COLS - n, TERM_COLS - 1);
            break;
        }
        case '@': {  // ICH - insert blank characters
            int n = std::min(param(0, 1), TERM_COLS - cur_col_);
            for (int c = TERM_COLS - 1; c >= cur_col_ + n; --c) {
                screen_[cur_row_][c] = screen_[cur_row_][c - n];
            }
            clearRow(cur_row_, cur_col_, cur_col_ + n - 1);
            break;
        }
        case 'X': {  // ECH - erase characters
            int n = std::min(param(0, 1), TERM_COLS - cur_col_);
            clearRow(cur_row_, cur_col_, cur_col_ + n - 1);
            break;
        }
        case 'S': {  // SU - scroll up
            int n = std::min(param(0, 1), TERM_ROWS);
            for (int i = 0; i < n; ++i) scrollRegionUp();
            break;
        }
        case 'T': {  // SD - scroll down
            int n = std::min(param(0, 1), TERM_ROWS);
            for (int i = 0; i < n; ++i) scrollRegionDown();
            break;
        }
        case 'm':  // SGR
            executeSgr();
            break;
        case 's':
            saved_row_ = cur_row_;
            saved_col_ = cur_col_;
            break;
        case 'u':
            cur_row_ = saved_row_;
            cur_col_ = saved_col_;
            clampCursor();
            break;
        default:
            break;
    }
}

void TerminalView::executeSgr() {
    if (csi_param_count_ == 0) {
        resetAttributes();
        return;
    }

    for (int i = 0; i < csi_param_count_; ++i) {
        int p = csi_params_[i];
        if (p == 0) {
            resetAttributes();
        } else if (p == 1) {
            cur_attrs_ |= ATTR_BOLD;
        } else if (p == 7) {
            cur_attrs_ |= ATTR_REVERSE;
        } else if (p == 22) {
            cur_attrs_ &= ~ATTR_BOLD;
        } else if (p == 27) {
            cur_attrs_ &= ~ATTR_REVERSE;
        } else if (p >= 30 && p <= 37) {
            cur_fg_ = static_cast<uint8_t>(p - 30);
        } else if (p == 39) {
            cur_fg_ = 7;
        } else if (p >= 40 && p <= 47) {
            cur_bg_ = static_cast<uint8_t>(p - 40);
        } else if (p == 49) {
            cur_bg_ = 0;
        } else if (p >= 90 && p <= 97) {
            cur_fg_ = static_cast<uint8_t>(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            cur_bg_ = static_cast<uint8_t>(p - 100 + 8);
        } else if (p == 38 || p == 48) {
            bool is_fg = (p == 38);
            if (i + 2 < csi_param_count_ && csi_params_[i + 1] == 5) {
                uint8_t mapped = map256Color(csi_params_[i + 2]);
                if (is_fg) cur_fg_ = mapped;
                else cur_bg_ = mapped;
                i += 2;
            } else if (i + 4 < csi_param_count_ && csi_params_[i + 1] == 2) {
                // Approximate 24-bit RGB to 16-color ANSI
                int r = csi_params_[i + 2];
                int g = csi_params_[i + 3];
                int b = csi_params_[i + 4];
                uint8_t idx = ((r > 127) ? 1 : 0) | ((g > 127) ? 2 : 0) | ((b > 127) ? 4 : 0);
                if (r > 192 || g > 192 || b > 192) idx |= 8;
                if (is_fg) cur_fg_ = idx;
                else cur_bg_ = idx;
                i += 4;
            }
        }
    }
}

void TerminalView::putChar(char ch) {
    if (cur_col_ >= TERM_COLS) {
        cur_col_ = 0;
        lineFeed();
    }
    screen_[cur_row_][cur_col_] = Cell{ch, cur_fg_, cur_bg_, cur_attrs_};
    cur_col_++;
    if (scroll_offset_ != 0) {
        scroll_offset_ = 0;
    }
}

void TerminalView::lineFeed() {
    if (cur_row_ < TERM_ROWS - 1) {
        cur_row_++;
    } else {
        scrollRegionUp();
    }
}

void TerminalView::reverseIndex() {
    if (cur_row_ > 0) {
        cur_row_--;
    } else {
        scrollRegionDown();
    }
}

void TerminalView::scrollRegionUp() {
    // Save top row into scrollback ring buffer
    std::memcpy(scrollback_[sb_head_], screen_[0], sizeof(screen_[0]));
    sb_head_ = (sb_head_ + 1) % SCROLLBACK_LINES;
    if (sb_count_ < SCROLLBACK_LINES) {
        sb_count_++;
    }

    for (int r = 0; r < TERM_ROWS - 1; ++r) {
        std::memcpy(screen_[r], screen_[r + 1], sizeof(screen_[r]));
    }
    clearRow(TERM_ROWS - 1);
}

void TerminalView::scrollRegionDown() {
    for (int r = TERM_ROWS - 1; r > 0; --r) {
        std::memcpy(screen_[r], screen_[r - 1], sizeof(screen_[r]));
    }
    clearRow(0);
}

void TerminalView::clearRow(int row, int from_col, int to_col) {
    if (row < 0 || row >= TERM_ROWS) return;
    from_col = std::max(0, from_col);
    to_col = std::min(TERM_COLS - 1, to_col);
    for (int c = from_col; c <= to_col; ++c) {
        screen_[row][c] = Cell{' ', 7, 0, 0};
    }
}

void TerminalView::resetAttributes() {
    cur_fg_ = 7;
    cur_bg_ = 0;
    cur_attrs_ = 0;
}

void TerminalView::clampCursor() {
    if (cur_row_ < 0) cur_row_ = 0;
    if (cur_row_ >= TERM_ROWS) cur_row_ = TERM_ROWS - 1;
    if (cur_col_ < 0) cur_col_ = 0;
    if (cur_col_ >= TERM_COLS) cur_col_ = TERM_COLS - 1;
}

uint8_t TerminalView::map256Color(int idx) const {
    if (idx < 0) return 7;
    if (idx < 16) return static_cast<uint8_t>(idx);
    if (idx < 232) {
        int c = idx - 16;
        int r = c / 36;
        int g = (c / 6) % 6;
        int b = c % 6;
        uint8_t out = ((r >= 2) ? 1 : 0) | ((g >= 2) ? 2 : 0) | ((b >= 2) ? 4 : 0);
        if (r >= 4 || g >= 4 || b >= 4) out |= 8;
        return out;
    }
    int gray = idx - 232;
    if (gray < 6) return 0;
    if (gray < 12) return 8;
    if (gray < 18) return 7;
    return 15;
}

uint16_t TerminalView::paletteColor(uint8_t idx) const {
    return kAnsiPalette[idx & 0x0F];
}

void TerminalView::drawTerminalGrid() {
    LovyanGFX& gfx = canvas_ready_ ? static_cast<LovyanGFX&>(canvas_)
                                   : static_cast<LovyanGFX&>(M5Cardputer.Display);

    for (int r = 0; r < TERM_ROWS; ++r) {
        const Cell* row_cells = nullptr;
        int virtual_row = r - scroll_offset_;
        if (virtual_row >= 0) {
            row_cells = screen_[virtual_row];
        } else {
            int sb_back = -virtual_row;  // 1 = most recent scrollback line
            int sb_idx = (sb_head_ - sb_back + SCROLLBACK_LINES) % SCROLLBACK_LINES;
            row_cells = scrollback_[sb_idx];
        }

        int y = r * FONT_HEIGHT;
        for (int c = 0; c < TERM_COLS; ++c) {
            const Cell& cell = row_cells[c];
            uint8_t fg_idx = cell.fg;
            uint8_t bg_idx = cell.bg;

            if ((cell.attrs & ATTR_BOLD) && fg_idx < 8) {
                fg_idx += 8;
            }
            if (cell.attrs & ATTR_REVERSE) {
                std::swap(fg_idx, bg_idx);
            }

            bool is_cursor = (scroll_offset_ == 0 && cursor_visible_ && cursor_blink_on_ &&
                              r == cur_row_ && c == cur_col_);
            if (is_cursor) {
                std::swap(fg_idx, bg_idx);
                if (fg_idx == bg_idx) {
                    bg_idx = 15;
                    fg_idx = 0;
                }
            }

            uint16_t fg16 = paletteColor(fg_idx);
            uint16_t bg16 = paletteColor(bg_idx);
            int x = c * FONT_WIDTH;

            gfx.fillRect(x, y, FONT_WIDTH, FONT_HEIGHT, bg16);
            if (cell.ch != ' ') {
                // Use setTextColor + 3-arg drawChar(uniCode, x, y); M5GFX's 6-arg drawChar swaps fg/bg.
                gfx.setTextColor(fg16, bg16);
                gfx.drawChar(static_cast<uint16_t>(static_cast<uint8_t>(cell.ch)), x, y);
            }
        }
    }
}

void TerminalView::drawStatusBar(const StatusInfo& status) {
    LovyanGFX& gfx = canvas_ready_ ? static_cast<LovyanGFX&>(canvas_)
                                   : static_cast<LovyanGFX&>(M5Cardputer.Display);

    constexpr uint16_t kBarBg = 0x18E3;     // Dark slate
    constexpr uint16_t kBarBorder = 0x4208; // Subtle divider
    constexpr uint16_t kTextFg = 0xFFFF;
    constexpr uint16_t kGoodFg = 0x07E0;
    constexpr uint16_t kWarnFg = 0xFFE0;
    constexpr uint16_t kErrFg = 0xF800;

    gfx.fillRect(0, STATUS_BAR_Y, SCREEN_WIDTH, STATUS_BAR_HEIGHT, kBarBg);
    gfx.drawFastHLine(0, STATUS_BAR_Y, SCREEN_WIDTH, kBarBorder);

    int y = STATUS_BAR_Y + 4;  // Center 8px font in 15px bar (1px border + 3px pad)
    gfx.setTextColor(kTextFg, kBarBg);

    // Left: Wi-Fi + SSH state/host
    char left_buf[28];
    const char* ssh_tag = "IDLE";
    uint16_t ssh_col = kTextFg;
    switch (status.ssh_state) {
        case SshState::DISCONNECTED:
            ssh_tag = "DISC";
            ssh_col = kWarnFg;
            break;
        case SshState::CONNECTING:
            ssh_tag = "CONN";
            ssh_col = kWarnFg;
            break;
        case SshState::HOST_VERIFY:
            ssh_tag = "TOFU";
            ssh_col = kWarnFg;
            break;
        case SshState::AUTHENTICATING:
            ssh_tag = "AUTH";
            ssh_col = kWarnFg;
            break;
        case SshState::CONNECTED:
            ssh_tag = "SSH";
            ssh_col = kGoodFg;
            break;
        case SshState::ERROR:
            ssh_tag = "ERR";
            ssh_col = kErrFg;
            break;
    }

    if (scroll_offset_ > 0) {
        std::snprintf(left_buf, sizeof(left_buf), "SCRL-%d", scroll_offset_);
        gfx.setTextColor(kWarnFg, kBarBg);
        gfx.drawString(left_buf, 2, y);
    } else {
        uint16_t wifi_col = status.wifi_connected ? kGoodFg : kErrFg;
        gfx.setTextColor(wifi_col, kBarBg);
        gfx.drawString(status.wifi_connected ? "W" : "!", 2, y);

        gfx.setTextColor(ssh_col, kBarBg);
        if (status.active_host[0] != '\0') {
            std::snprintf(left_buf, sizeof(left_buf), " %s:%.16s", ssh_tag, status.active_host);
        } else {
            std::snprintf(left_buf, sizeof(left_buf), " %s", ssh_tag);
        }
        gfx.drawString(left_buf, 8, y);
    }

    // Right: Modifiers + Battery %
    char right_buf[20];
    std::snprintf(right_buf, sizeof(right_buf), "%s%s%s%3u%%",
                  status.fn_active ? "F " : "",
                  status.ctrl_active ? "C " : "",
                  status.alt_active ? "A " : "",
                  static_cast<unsigned>(status.battery_pct));
    int rw = static_cast<int>(std::strlen(right_buf)) * FONT_WIDTH;
    gfx.setTextColor((status.fn_active || status.ctrl_active || status.alt_active) ? kWarnFg : kTextFg, kBarBg);
    gfx.drawString(right_buf, SCREEN_WIDTH - rw - 2, y);
}

}  // namespace esp_cpa_ssh
