// SPDX-License-Identifier: MIT

#include "terminal_screen.h"

#include "asset_manager.h"
#include "theme.h"

#include <filesystem>
#include <string>

namespace screen {
namespace {

constexpr int32_t kStatusHeight = 18;
constexpr int32_t kInputHeight = 22;
constexpr int32_t kTextPadding = 3;
constexpr std::size_t kInputTail = 34;

// A fixed-width face keeps RTTY text and the menu columns aligned. These ship
// with most Debian images; without one the system UI font is used.
constexpr const char* kMonospaceFonts[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
};

const lv_font_t* load_text_font(app::AssetManager& assets, uint32_t size, const lv_font_t* fallback) {
    for (const char* path : kMonospaceFonts) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error)) {
            if (auto* font = assets.load_font(path, size)) {
                return font;
            }
        }
    }
    if (auto* font = assets.load_standard_font(size, app::StandardFontWeight::Regular)) {
        return font;
    }
    return fallback;
}

lv_obj_t* make_box(lv_obj_t* parent) {
    auto* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    return box;
}

} // namespace

TerminalScreen::TerminalScreen(app::AssetManager& assets) : assets_(assets) {
    text_font_ = load_text_font(assets_, 14, &lv_font_montserrat_14);
    small_text_font_ = load_text_font(assets_, 11, &lv_font_montserrat_12);
    ui_font_ = load_text_font(assets_, 11, &lv_font_montserrat_12);

    root_ = make_box(nullptr);
    lv_obj_set_size(root_, view::kScreenWidth, view::kScreenHeight);

    status_bar_ = make_box(root_);
    lv_obj_set_size(status_bar_, view::kScreenWidth, kStatusHeight);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_LEFT, 0, 0);

    status_left_ = lv_label_create(status_bar_);
    lv_label_set_long_mode(status_left_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(status_left_, ui_font_, 0);
    lv_obj_align(status_left_, LV_ALIGN_LEFT_MID, 5, 0);

    status_right_ = lv_label_create(status_bar_);
    lv_obj_set_style_text_font(status_right_, ui_font_, 0);
    lv_obj_align(status_right_, LV_ALIGN_RIGHT_MID, -5, 0);

    text_area_ = make_box(root_);
    lv_obj_set_size(text_area_,
                    view::kScreenWidth,
                    view::kScreenHeight - kStatusHeight - kInputHeight);
    lv_obj_align(text_area_, LV_ALIGN_TOP_LEFT, 0, kStatusHeight);
    lv_obj_set_style_pad_all(text_area_, kTextPadding, 0);
    lv_obj_add_flag(text_area_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(text_area_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(text_area_, LV_SCROLLBAR_MODE_OFF);

    text_ = lv_label_create(text_area_);
    lv_label_set_long_mode(text_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text_, view::kScreenWidth - 2 * kTextPadding);
    lv_label_set_text(text_, "");

    input_bar_ = make_box(root_);
    lv_obj_set_size(input_bar_, view::kScreenWidth, kInputHeight);
    lv_obj_align(input_bar_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_border_width(input_bar_, 1, 0);
    lv_obj_set_style_border_side(input_bar_, LV_BORDER_SIDE_TOP, 0);

    input_ = lv_label_create(input_bar_);
    lv_label_set_long_mode(input_, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(input_, view::kScreenWidth - 10);
    lv_obj_align(input_, LV_ALIGN_LEFT_MID, 5, 0);

    overlay_ = make_box(root_);
    lv_obj_set_size(overlay_, view::kScreenWidth, view::kScreenHeight);
    lv_obj_align(overlay_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_hor(overlay_, 8, 0);
    lv_obj_set_style_pad_ver(overlay_, 6, 0);
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);

    overlay_text_ = lv_label_create(overlay_);
    lv_label_set_long_mode(overlay_text_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(overlay_text_, view::kScreenWidth - 16);
    lv_obj_set_style_text_font(overlay_text_, ui_font_, 0);
    lv_obj_set_style_text_line_space(overlay_text_, 1, 0);
}

TerminalScreen::~TerminalScreen() {
    if (root_ && lv_obj_is_valid(root_)) {
        lv_obj_delete(root_);
    }
}

void TerminalScreen::apply_fonts(bool small_font) {
    if (fonts_applied_ && small_font == small_font_) {
        return;
    }
    fonts_applied_ = true;
    small_font_ = small_font;
    lv_obj_set_style_text_font(text_, small_font ? small_text_font_ : text_font_, 0);
    lv_obj_set_style_text_font(input_, text_font_, 0);
}

void TerminalScreen::scroll_text(rtty::Terminal& terminal) {
    const int lines = terminal.take_scroll_lines();
    const bool follow = terminal.take_follow_request();

    lv_obj_update_layout(text_area_);
    if (follow) {
        following_ = true;
    }
    else if (lines != 0) {
        const auto* font = small_font_ ? small_text_font_ : text_font_;
        const int32_t step = lv_font_get_line_height(font) * lines;
        // Positive distances move the content down, i.e. towards older text.
        lv_obj_scroll_by_bounded(text_area_, 0, -step, LV_ANIM_OFF);
        following_ = lv_obj_get_scroll_bottom(text_area_) <= 0;
    }

    if (following_) {
        lv_obj_scroll_to_y(text_area_, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void TerminalScreen::render(rtty::Terminal& terminal, bool dark_mode, bool small_font) {
    const auto colors = view::palette(dark_mode);
    const auto status = terminal.status();
    apply_fonts(small_font);

    // Status bar. A transmission in progress turns the whole bar red so it
    // cannot be missed outdoors.
    std::string left;
    std::string right;
    lv_color_t bar_color = colors.bar;
    lv_color_t bar_text = colors.text;
    if (status.link != rtty::StatusView::Link::Ready) {
        left = status.problem;
        bar_color = colors.warning;
        bar_text = lv_color_black();
    }
    else {
        left = status.frequency;
        if (!status.mode.empty()) {
            left += "  " + status.mode;
        }
        if (!status.mode_sends_text) {
            left += " (no text TX)";
            bar_color = colors.warning;
            bar_text = lv_color_black();
        }
        if (!status.dx_call.empty()) {
            right += status.dx_call + "  ";
        }
        right += status.line_mode ? "LINE  " : "IMM  ";
        if (status.transmitting) {
            right += "TX";
            bar_color = colors.error;
            bar_text = lv_color_white();
        }
        else {
            right += "RX";
        }
        if (status.queued > 0) {
            right += " " + std::to_string(status.queued);
        }
    }
    lv_obj_set_style_bg_color(status_bar_, bar_color, 0);
    lv_obj_set_style_text_color(status_left_, bar_text, 0);
    lv_obj_set_style_text_color(status_right_, bar_text, 0);
    lv_label_set_text(status_right_, right.c_str());
    lv_obj_update_layout(status_right_);
    lv_obj_set_width(status_left_,
                     view::kScreenWidth - 15 - (right.empty() ? 0 : lv_obj_get_width(status_right_)));
    lv_label_set_text(status_left_, left.c_str());

    // Received and sent text.
    lv_obj_set_style_bg_color(root_, colors.background, 0);
    lv_obj_set_style_bg_color(text_area_, colors.background, 0);
    lv_obj_set_style_text_color(text_, colors.text, 0);
    if (terminal.scrollback() != shown_text_) {
        shown_text_ = terminal.scrollback();
        // A line break at the very end would only show as an empty last row.
        std::string visible = shown_text_;
        if (!visible.empty() && visible.back() == '\n') {
            visible.pop_back();
        }
        lv_label_set_text(text_, visible.c_str());
    }
    scroll_text(terminal);

    // Input line.
    lv_obj_set_style_bg_color(input_bar_, colors.surface, 0);
    lv_obj_set_style_border_color(input_bar_, colors.border, 0);
    std::string input;
    if (status.line_mode) {
        const auto& line = terminal.input_line();
        input = "> ";
        if (line.size() > kInputTail) {
            input += "<" + line.substr(line.size() - kInputTail);
        }
        else {
            input += line;
        }
        input += "_";
        lv_obj_set_style_text_color(input_, colors.text, 0);
    }
    else {
        input = "Keys are sent as typed. ENTER ends.";
        lv_obj_set_style_text_color(input_, colors.info, 0);
    }
    lv_label_set_text(input_, input.c_str());

    // Overlays cover the whole screen.
    if (terminal.overlay() == rtty::Overlay::None) {
        lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_set_style_bg_color(overlay_, colors.surface, 0);
        lv_obj_set_style_text_color(overlay_text_, colors.text, 0);
        lv_label_set_text(overlay_text_, terminal.overlay_text().c_str());
        lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace screen
