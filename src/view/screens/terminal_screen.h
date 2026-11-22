// SPDX-License-Identifier: MIT
//
// The one screen of ZeroRTTY: status bar, received/sent text, input line and
// the full-screen overlays (menu, setup, edit, help).

#pragma once

#include "lvgl.h"
#include "terminal.h"

#include <string>

namespace app {
class AssetManager;
}

namespace screen {

class TerminalScreen {
public:
    explicit TerminalScreen(app::AssetManager& assets);
    ~TerminalScreen();

    TerminalScreen(const TerminalScreen&) = delete;
    TerminalScreen& operator=(const TerminalScreen&) = delete;

    lv_obj_t* root() const { return root_; }

    // Redraws from the terminal's state. Cheap enough to call on every change.
    void render(rtty::Terminal& terminal, bool dark_mode, bool small_font);

private:
    void apply_fonts(bool small_font);
    void scroll_text(rtty::Terminal& terminal);

    app::AssetManager& assets_;
    lv_obj_t* root_{nullptr};
    lv_obj_t* status_bar_{nullptr};
    lv_obj_t* status_left_{nullptr};
    lv_obj_t* status_right_{nullptr};
    lv_obj_t* text_area_{nullptr};
    lv_obj_t* text_{nullptr};
    lv_obj_t* input_bar_{nullptr};
    lv_obj_t* input_{nullptr};
    lv_obj_t* overlay_{nullptr};
    lv_obj_t* overlay_text_{nullptr};

    const lv_font_t* text_font_{nullptr};
    const lv_font_t* small_text_font_{nullptr};
    const lv_font_t* ui_font_{nullptr};
    bool fonts_applied_{false};
    bool small_font_{false};
    bool following_{true};
    std::string shown_text_;
};

} // namespace screen
