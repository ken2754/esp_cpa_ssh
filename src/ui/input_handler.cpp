#include "input_handler.h"

#include <Arduino.h>

namespace esp_cpa_ssh {

bool InputHandler::shouldEmitKeyEvent() {
    auto& kb = M5Cardputer.Keyboard;
    auto& ks = kb.keysState();

    fn_active_ = ks.fn;
    ctrl_active_ = (ks.ctrl || ks.opt);
    alt_active_ = ks.alt;

    bool has_actionable = ks.enter || ks.del || ks.tab || ks.space || !ks.word.empty();

    if (kb.isChange()) {
        if (kb.isPressed() && has_actionable) {
            key_held_ = true;
            key_down_ms_ = millis();
            last_repeat_ms_ = key_down_ms_;
            return true;
        }
        if (!has_actionable) {
            key_held_ = false;
        }
        return false;
    }

    if (key_held_ && kb.isPressed() && has_actionable) {
        uint32_t now = millis();
        if (now - key_down_ms_ >= REPEAT_DELAY_MS && now - last_repeat_ms_ >= REPEAT_RATE_MS) {
            last_repeat_ms_ = now;
            return true;
        }
    } else if (!kb.isPressed()) {
        key_held_ = false;
    }

    return false;
}

size_t InputHandler::appendBytes(uint8_t* dst, size_t pos, size_t max_len, const char* src, size_t n) {
    for (size_t i = 0; i < n && pos < max_len; ++i) {
        dst[pos++] = static_cast<uint8_t>(src[i]);
    }
    return pos;
}

size_t InputHandler::poll(uint8_t* out_buf, size_t max_len, bool& out_menu_requested, int& out_scroll_delta) {
    out_menu_requested = false;
    out_scroll_delta = 0;

    if (M5Cardputer.BtnA.wasPressed()) {
        out_menu_requested = true;
    }

    if (!shouldEmitKeyEvent()) {
        return 0;
    }

    return translateTerminal(out_buf, max_len, out_menu_requested, out_scroll_delta);
}

size_t InputHandler::translateTerminal(uint8_t* out_buf, size_t max_len,
                                       bool& out_menu_requested, int& out_scroll_delta) {
    auto& ks = M5Cardputer.Keyboard.keysState();
    size_t pos = 0;

    bool ctrl = (ks.ctrl || ks.opt);
    bool fn = ks.fn;
    bool alt = ks.alt;
    bool shift = ks.shift;

    if (ks.enter) {
        pos = appendBytes(out_buf, pos, max_len, "\r", 1);
    }
    if (ks.tab) {
        if (shift) {
            pos = appendBytes(out_buf, pos, max_len, "\x1b[Z", 3);
        } else {
            pos = appendBytes(out_buf, pos, max_len, "\t", 1);
        }
    }
    if (ks.del) {
        if (fn) {
            pos = appendBytes(out_buf, pos, max_len, "\x1b[3~", 4);
        } else {
            pos = appendBytes(out_buf, pos, max_len, "\x7f", 1);
        }
    }
    if (ks.space) {
        if (ctrl) {
            pos = appendBytes(out_buf, pos, max_len, "\x00", 1);
        } else {
            pos = appendBytes(out_buf, pos, max_len, " ", 1);
        }
    }

    for (char c : ks.word) {
        if (fn) {
            // Cardputer Fn combos
            switch (c) {
                case ';':
                case ':':
                    if (shift) {
                        out_scroll_delta += 3;
                    } else {
                        pos = appendBytes(out_buf, pos, max_len, "\x1b[A", 3);
                    }
                    continue;
                case '.':
                case '>':
                    if (shift) {
                        out_scroll_delta -= 3;
                    } else {
                        pos = appendBytes(out_buf, pos, max_len, "\x1b[B", 3);
                    }
                    continue;
                case ',':
                case '<':
                    pos = appendBytes(out_buf, pos, max_len, "\x1b[D", 3);
                    continue;
                case '/':
                case '?':
                    pos = appendBytes(out_buf, pos, max_len, "\x1b[C", 3);
                    continue;
                case '`':
                case '~':
                    pos = appendBytes(out_buf, pos, max_len, "\x1b", 1);
                    continue;
                case '[':
                case '{':
                    pos = appendBytes(out_buf, pos, max_len, "\x1b[H", 3);
                    continue;
                case ']':
                case '}':
                    pos = appendBytes(out_buf, pos, max_len, "\x1b[F", 3);
                    continue;
                case '\'':
                case '"':
                    pos = appendBytes(out_buf, pos, max_len, "\x1b[5~", 4);
                    continue;
                case '\\':
                case '|':
                    pos = appendBytes(out_buf, pos, max_len, "\x1b[6~", 4);
                    continue;
                case '1': pos = appendBytes(out_buf, pos, max_len, "\x1bOP", 3); continue;
                case '2': pos = appendBytes(out_buf, pos, max_len, "\x1bOQ", 3); continue;
                case '3': pos = appendBytes(out_buf, pos, max_len, "\x1bOR", 3); continue;
                case '4': pos = appendBytes(out_buf, pos, max_len, "\x1bOS", 3); continue;
                case '5': pos = appendBytes(out_buf, pos, max_len, "\x1b[15~", 5); continue;
                case '6': pos = appendBytes(out_buf, pos, max_len, "\x1b[17~", 5); continue;
                case '7': pos = appendBytes(out_buf, pos, max_len, "\x1b[18~", 5); continue;
                case '8': pos = appendBytes(out_buf, pos, max_len, "\x1b[19~", 5); continue;
                case '9': pos = appendBytes(out_buf, pos, max_len, "\x1b[20~", 5); continue;
                case '0': pos = appendBytes(out_buf, pos, max_len, "\x1b[21~", 5); continue;
                default:
                    break;
            }
        }

        if (ctrl) {
            if (c >= 'a' && c <= 'z') {
                char ctrl_ch = static_cast<char>(c - 'a' + 1);
                pos = appendBytes(out_buf, pos, max_len, &ctrl_ch, 1);
                continue;
            }
            if (c >= 'A' && c <= 'Z') {
                char ctrl_ch = static_cast<char>(c - 'A' + 1);
                pos = appendBytes(out_buf, pos, max_len, &ctrl_ch, 1);
                continue;
            }
            if (c == '[') {
                pos = appendBytes(out_buf, pos, max_len, "\x1b", 1);
                continue;
            }
            if (c == '\\') {
                pos = appendBytes(out_buf, pos, max_len, "\x1c", 1);
                continue;
            }
            if (c == ']') {
                out_menu_requested = true;
                continue;
            }
            if (c == '^' || c == '6') {
                pos = appendBytes(out_buf, pos, max_len, "\x1e", 1);
                continue;
            }
            if (c == '_' || c == '-') {
                pos = appendBytes(out_buf, pos, max_len, "\x1f", 1);
                continue;
            }
        }

        if (alt) {
            if (c == ';') {
                out_scroll_delta += 3;
                continue;
            }
            if (c == '.') {
                out_scroll_delta -= 3;
                continue;
            }
            pos = appendBytes(out_buf, pos, max_len, "\x1b", 1);
        }

        pos = appendBytes(out_buf, pos, max_len, &c, 1);
    }

    return pos;
}

MenuKey InputHandler::pollMenuKey(char& out_char) {
    out_char = '\0';

    if (M5Cardputer.BtnA.wasPressed()) {
        return MenuKey::ESC;
    }

    if (!shouldEmitKeyEvent()) {
        return MenuKey::NONE;
    }

    auto& ks = M5Cardputer.Keyboard.keysState();
    if (ks.enter) return MenuKey::ENTER;
    if (ks.del) return MenuKey::BACKSPACE;
    if (ks.space) {
        out_char = ' ';
        return MenuKey::CHAR;
    }

    if (!ks.word.empty()) {
        char c = ks.word[0];
        if (ks.fn) {
            if (c == ';' || c == ':') return MenuKey::UP;
            if (c == '.' || c == '>') return MenuKey::DOWN;
            if (c == ',' || c == '<') return MenuKey::LEFT;
            if (c == '/' || c == '?') return MenuKey::RIGHT;
            if (c == '`' || c == '~') return MenuKey::ESC;
        }
        if ((ks.ctrl || ks.opt) && c == '[') {
            return MenuKey::ESC;
        }
        out_char = c;
        return MenuKey::CHAR;
    }

    return MenuKey::NONE;
}

}  // namespace esp_cpa_ssh
