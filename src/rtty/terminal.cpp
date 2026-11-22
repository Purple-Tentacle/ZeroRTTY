// SPDX-License-Identifier: MIT

#include "terminal.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace rtty {
namespace {

constexpr std::size_t kMaxInput = 200;
constexpr std::size_t kMaxScrollback = 4000;
constexpr std::size_t kMaxCall = 12;
constexpr std::size_t kMaxDevice = 64;
constexpr std::size_t kMaxMacro = 100;
constexpr std::size_t kMaxSerialDigits = 5;
constexpr int kMaxSerial = 99999;
constexpr std::size_t kMenuMacroPreview = 38;
constexpr int kScrollStep = 3;
constexpr int kPageStep = 8;
constexpr int kBaudRates[] = {4800, 9600, 19200, 38400};

char to_upper(char character) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
}

bool is_printable(char character) {
    const auto value = static_cast<unsigned char>(character);
    return value >= 32 && value <= 126;
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
    std::size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

bool contains_call_placeholder(const std::string& text) {
    return text.find("[CALL]") != std::string::npos || text.find("{CALL}") != std::string::npos;
}

bool contains_serial_placeholder(const std::string& text) {
    return text.find("[NR]") != std::string::npos || text.find("{NR}") != std::string::npos;
}

// Contest style: at least three digits, "001".
std::string format_serial(int serial) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%03d", serial);
    return buffer;
}

std::string format_frequency(long long hz) {
    if (hz <= 0) {
        return {};
    }
    char buffer[32];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%lld.%03lld.%02lld",
                  hz / 1000000,
                  (hz / 1000) % 1000,
                  (hz / 10) % 100);
    return buffer;
}

std::string preview(const std::string& text, std::size_t width) {
    if (text.size() <= width) {
        return text;
    }
    return text.substr(0, width - 2) + "..";
}

} // namespace

void LineEdit::set(const std::string& text) {
    text_ = text;
    cursor_ = text_.size();
}

bool LineEdit::insert(char character, std::size_t limit) {
    if (text_.size() >= limit) {
        return false;
    }
    text_.insert(cursor_, 1, character);
    ++cursor_;
    return true;
}

bool LineEdit::backspace() {
    if (cursor_ == 0) {
        return false;
    }
    --cursor_;
    text_.erase(cursor_, 1);
    return true;
}

bool LineEdit::erase() {
    if (cursor_ >= text_.size()) {
        return false;
    }
    text_.erase(cursor_, 1);
    return true;
}

bool LineEdit::left() {
    if (cursor_ == 0) {
        return false;
    }
    --cursor_;
    return true;
}

bool LineEdit::right() {
    if (cursor_ >= text_.size()) {
        return false;
    }
    ++cursor_;
    return true;
}

bool LineEdit::home() {
    if (cursor_ == 0) {
        return false;
    }
    cursor_ = 0;
    return true;
}

bool LineEdit::end() {
    if (cursor_ == text_.size()) {
        return false;
    }
    cursor_ = text_.size();
    return true;
}

std::string LineEdit::with_cursor() const {
    if (cursor_ >= text_.size()) {
        return text_ + "_";
    }
    return text_.substr(0, cursor_) + "|" + text_.substr(cursor_);
}

Terminal::Terminal(Settings& settings, Kx2Link& link, Hooks hooks)
    : settings_(settings), link_(link), hooks_(std::move(hooks)) {}

LinkOptions Terminal::link_options() const {
    LinkOptions options;
    options.device = settings_.device;
    options.baud = settings_.baud;
    options.extra_tx_characters = settings_.extra_tx_characters;
    return options;
}

void Terminal::settings_changed() {
    link_.reconfigure(link_options());
    if (hooks_.settings_changed) {
        hooks_.settings_changed();
    }
    changed();
}

void Terminal::notify(const std::string& message) {
    notice_ = message;
}

std::string Terminal::take_notice() {
    std::string result;
    result.swap(notice_);
    return result;
}

int Terminal::take_scroll_lines() {
    return std::exchange(scroll_lines_, 0);
}

bool Terminal::take_follow_request() {
    return std::exchange(follow_request_, false);
}

void Terminal::append_scrollback(Direction direction, const std::string& text) {
    if (text.empty()) {
        return;
    }

    if (direction != last_direction_) {
        if (!scrollback_.empty() && scrollback_.back() != '\n') {
            scrollback_.push_back('\n');
        }
        if (direction == Direction::Sent) {
            scrollback_ += "> ";
        }
        last_direction_ = direction;
    }
    scrollback_ += text;

    if (scrollback_.size() > kMaxScrollback) {
        std::size_t cut = scrollback_.size() - kMaxScrollback;
        const auto line_end = scrollback_.find('\n', cut);
        if (line_end != std::string::npos && line_end - cut < 200) {
            cut = line_end + 1;
        }
        scrollback_.erase(0, cut);
    }
    changed();
}

void Terminal::tick() {
    append_scrollback(Direction::Sent, link_.take_sent());
    append_scrollback(Direction::Received, link_.take_received());

    const auto status = link_.status();
    if (status.port_open != link_status_.port_open ||
        status.radio_responding != link_status_.radio_responding ||
        status.transmitting != link_status_.transmitting ||
        status.frequency_hz != link_status_.frequency_hz || status.mode != link_status_.mode ||
        status.data_sub_mode != link_status_.data_sub_mode ||
        status.queued != link_status_.queued || status.port_error != link_status_.port_error) {
        changed();
    }
    link_status_ = status;
}

bool Terminal::macros_use_serial() const {
    for (const auto& macro : settings_.macros) {
        if (contains_serial_placeholder(macro)) {
            return true;
        }
    }
    return false;
}

StatusView Terminal::status() const {
    StatusView view;
    view.line_mode = settings_.line_mode;
    view.dx_call = dx_call_;
    if (macros_use_serial()) {
        view.serial = format_serial(settings_.serial);
    }
    view.queued = link_status_.queued;
    view.transmitting = link_status_.transmitting;

    if (!link_status_.port_open) {
        view.link = StatusView::Link::NoPort;
        view.problem = "No port " + settings_.device;
        if (!link_status_.port_error.empty()) {
            view.problem += ": " + link_status_.port_error;
        }
        return view;
    }
    if (!link_status_.radio_responding) {
        view.link = StatusView::Link::NoRadio;
        view.problem = "No reply from KX2 at " + std::to_string(settings_.baud) + " baud";
        return view;
    }

    view.link = StatusView::Link::Ready;
    view.frequency = format_frequency(link_status_.frequency_hz);
    view.mode = mode_label(link_status_.mode, link_status_.data_sub_mode);
    view.mode_sends_text = link_status_.mode == 0 ||
                           rtty::mode_sends_text(link_status_.mode, link_status_.data_sub_mode);
    return view;
}

void Terminal::handle_key(const Key& key) {
    switch (overlay_) {
        case Overlay::None:
            handle_main_key(key);
            break;
        case Overlay::Menu:
            handle_menu_key(key);
            break;
        case Overlay::Setup:
            handle_setup_key(key);
            break;
        case Overlay::Edit:
            handle_edit_key(key);
            break;
        case Overlay::Help:
            overlay_ = Overlay::None;
            changed();
            break;
    }
}

void Terminal::toggle_line_mode() {
    settings_.line_mode = !settings_.line_mode;
    if (!settings_.line_mode && !input_.empty()) {
        // Nothing typed so far has been sent; do not send it behind the user's back.
        input_.clear();
        notify("Immediate mode - unsent line discarded");
    }
    else {
        notify(settings_.line_mode ? "Line mode: ENTER sends" : "Immediate mode: keys send at once");
    }
    settings_changed();
}

bool Terminal::ready_to_send() {
    link_status_ = link_.status();
    if (!link_status_.port_open || !link_status_.radio_responding) {
        notify("KX2 not connected - nothing sent");
        return false;
    }
    if (link_status_.mode != 0 &&
        !rtty::mode_sends_text(link_status_.mode, link_status_.data_sub_mode)) {
        notify("KX2 is in " + mode_label(link_status_.mode, link_status_.data_sub_mode) +
               " - TAB, D selects FSK-D");
        return false;
    }
    return true;
}

void Terminal::send_macro(std::size_t index) {
    if (index >= settings_.macros.size()) {
        return;
    }
    std::string text = settings_.macros[index];
    if (text.empty()) {
        notify("Macro " + std::to_string(index + 1) + " is empty");
        return;
    }
    if (contains_call_placeholder(text) && dx_call_.empty()) {
        notify("Set the DX call first (Fn+9)");
        return;
    }

    if (!ready_to_send()) {
        return;
    }

    replace_all(text, "[MY]", settings_.my_call);
    replace_all(text, "{MY}", settings_.my_call);
    replace_all(text, "[CALL]", dx_call_);
    replace_all(text, "{CALL}", dx_call_);
    const bool uses_serial = contains_serial_placeholder(text);
    replace_all(text, "[NR]", format_serial(settings_.serial));
    replace_all(text, "{NR}", format_serial(settings_.serial));

    if (link_.send_text(text + " ").empty()) {
        notify("Macro " + std::to_string(index + 1) + " has nothing to send");
        return;
    }
    link_.end_over();
    if (uses_serial) {
        serial_used_ = true;
    }
    follow_request_ = true;
    changed();
}

void Terminal::handle_main_key(const Key& key) {
    switch (key.kind) {
        case Key::Kind::Character: {
            const char character = to_upper(key.character);
            if (!is_sendable(character, settings_.extra_tx_characters)) {
                if (is_printable(key.character)) {
                    notify(std::string("'") + key.character + "' cannot be sent");
                }
                return;
            }
            follow_request_ = true;
            if (settings_.line_mode) {
                input_.insert(character, kMaxInput);
            }
            else if (ready_to_send()) {
                link_.send_text(std::string(1, character));
            }
            changed();
            return;
        }
        case Key::Kind::Backspace:
            if (!settings_.line_mode) {
                notify("Immediate mode: already sent");
            }
            else if (input_.backspace()) {
                changed();
            }
            return;
        case Key::Kind::Delete:
            if (settings_.line_mode && input_.erase()) {
                changed();
            }
            return;
        case Key::Kind::Left:
            if (settings_.line_mode && input_.left()) {
                changed();
            }
            return;
        case Key::Kind::Right:
            if (settings_.line_mode && input_.right()) {
                changed();
            }
            return;
        case Key::Kind::Home:
            if (settings_.line_mode && input_.home()) {
                changed();
            }
            return;
        case Key::Kind::Enter:
            follow_request_ = true;
            changed();
            if (settings_.line_mode && !input_.empty()) {
                // The line stays put if it cannot be sent, so nothing typed is lost.
                if (!ready_to_send()) {
                    return;
                }
                link_.send_text(input_.text() + " ");
                input_.clear();
            }
            link_.end_over();
            return;
        case Key::Kind::Escape:
            if (link_status_.queued > 0 || link_status_.transmitting) {
                link_.abort();
                notify("Transmission stopped");
            }
            else {
                notify("Hold ESC to exit");
            }
            return;
        case Key::Kind::Tab:
            overlay_ = Overlay::Menu;
            menu_edit_armed_ = false;
            changed();
            return;
        case Key::Kind::Function:
            if (key.number >= 1 && key.number <= static_cast<int>(kMacroCount)) {
                send_macro(static_cast<std::size_t>(key.number - 1));
            }
            else if (key.number == 9) {
                begin_edit(Field::DxCall, 0, Overlay::None);
            }
            else if (key.number == 10) {
                toggle_line_mode();
            }
            return;
        case Key::Kind::Up:
            scroll_lines_ -= kScrollStep;
            changed();
            return;
        case Key::Kind::Down:
            scroll_lines_ += kScrollStep;
            changed();
            return;
        case Key::Kind::PageUp:
            scroll_lines_ -= kPageStep;
            changed();
            return;
        case Key::Kind::PageDown:
            scroll_lines_ += kPageStep;
            changed();
            return;
        case Key::Kind::End:
            if (settings_.line_mode) {
                input_.end();
            }
            follow_request_ = true;
            changed();
            return;
        case Key::Kind::Help:
            overlay_ = Overlay::Help;
            changed();
            return;
    }
}

void Terminal::handle_menu_key(const Key& key) {
    const auto close = [this]() {
        overlay_ = Overlay::None;
        menu_edit_armed_ = false;
        changed();
    };

    if (key.kind == Key::Kind::Escape || key.kind == Key::Kind::Tab) {
        if (menu_edit_armed_) {
            menu_edit_armed_ = false;
            changed();
        }
        else {
            close();
        }
        return;
    }
    if (key.kind == Key::Kind::Help) {
        overlay_ = Overlay::Help;
        changed();
        return;
    }
    if (key.kind == Key::Kind::Function && key.number >= 1 &&
        key.number <= static_cast<int>(kMacroCount)) {
        close();
        send_macro(static_cast<std::size_t>(key.number - 1));
        return;
    }
    if (key.kind != Key::Kind::Character) {
        return;
    }

    const char character = to_upper(key.character);
    if (character >= '1' && character < static_cast<char>('1' + kMacroCount)) {
        const auto index = static_cast<std::size_t>(character - '1');
        if (menu_edit_armed_) {
            begin_edit(Field::Macro, index, Overlay::Menu);
        }
        else {
            close();
            send_macro(index);
        }
        return;
    }

    menu_edit_armed_ = false;
    switch (character) {
        case 'E':
            menu_edit_armed_ = true;
            changed();
            break;
        case 'M':
            toggle_line_mode();
            break;
        case 'C':
            begin_edit(Field::DxCall, 0, Overlay::None);
            break;
        case 'N':
            begin_edit(Field::Serial, 0, Overlay::Menu);
            break;
        case 'S':
            overlay_ = Overlay::Setup;
            changed();
            break;
        case 'D':
            link_.select_fsk_d();
            close();
            notify("KX2 set to DATA / FSK D");
            break;
        case 'X':
            scrollback_.clear();
            last_direction_ = Direction::None;
            close();
            break;
        default:
            changed();
            break;
    }
}

void Terminal::handle_setup_key(const Key& key) {
    if (key.kind == Key::Kind::Escape || key.kind == Key::Kind::Tab) {
        overlay_ = Overlay::None;
        changed();
        return;
    }
    if (key.kind != Key::Kind::Character) {
        return;
    }

    switch (key.character) {
        case '1':
            begin_edit(Field::MyCall, 0, Overlay::Setup);
            break;
        case '2':
            begin_edit(Field::Device, 0, Overlay::Setup);
            break;
        case '3': {
            std::size_t next = 0;
            const std::size_t count = sizeof(kBaudRates) / sizeof(kBaudRates[0]);
            for (std::size_t index = 0; index < count; ++index) {
                if (kBaudRates[index] == settings_.baud) {
                    next = (index + 1) % count;
                }
            }
            settings_.baud = kBaudRates[next];
            settings_changed();
            break;
        }
        case '4':
            settings_.small_font = !settings_.small_font;
            settings_changed();
            break;
        case '5':
            settings_.dark_mode = !settings_.dark_mode;
            settings_changed();
            break;
        default:
            break;
    }
}

void Terminal::begin_edit(Field field, std::size_t macro_index, Overlay return_to) {
    edit_field_ = field;
    edit_macro_ = macro_index;
    edit_return_ = return_to;
    menu_edit_armed_ = false;
    switch (field) {
        case Field::MyCall:
            edit_value_.set(settings_.my_call);
            break;
        case Field::DxCall:
            edit_value_.set(dx_call_);
            break;
        case Field::Device:
            edit_value_.set(settings_.device);
            break;
        case Field::Macro:
            edit_value_.set(settings_.macros[macro_index]);
            break;
        case Field::Serial:
            edit_value_.set(std::to_string(settings_.serial));
            break;
    }
    overlay_ = Overlay::Edit;
    changed();
}

void Terminal::commit_edit() {
    switch (edit_field_) {
        case Field::MyCall:
            if (edit_value_.empty()) {
                notify("The call cannot be empty");
                return;
            }
            settings_.my_call = edit_value_.text();
            settings_changed();
            break;
        case Field::DxCall:
            // A new station after the number has been given out is the next
            // QSO. Correcting the call before sending it changes nothing.
            if (serial_used_ && !edit_value_.empty() && edit_value_.text() != dx_call_ &&
                settings_.serial < kMaxSerial) {
                ++settings_.serial;
                serial_used_ = false;
                notify("Next serial: " + format_serial(settings_.serial));
                dx_call_ = edit_value_.text();
                settings_changed();
                break;
            }
            dx_call_ = edit_value_.text();
            break;
        case Field::Serial: {
            const int serial = edit_value_.empty() ? 0 : std::atoi(edit_value_.text().c_str());
            if (serial < 1 || serial > kMaxSerial) {
                notify("The serial must be 1 or more");
                return;
            }
            settings_.serial = serial;
            serial_used_ = false;
            settings_changed();
            break;
        }
        case Field::Device:
            if (edit_value_.empty()) {
                notify("The port cannot be empty");
                return;
            }
            settings_.device = edit_value_.text();
            settings_changed();
            break;
        case Field::Macro:
            settings_.macros[edit_macro_] = edit_value_.text();
            settings_changed();
            break;
    }
    overlay_ = edit_return_;
    changed();
}

void Terminal::handle_edit_key(const Key& key) {
    switch (key.kind) {
        case Key::Kind::Escape:
            overlay_ = edit_return_;
            changed();
            return;
        case Key::Kind::Enter:
            commit_edit();
            return;
        case Key::Kind::Backspace:
            if (edit_value_.backspace()) {
                changed();
            }
            return;
        case Key::Kind::Delete:
            if (edit_value_.erase()) {
                changed();
            }
            return;
        case Key::Kind::Left:
            if (edit_value_.left()) {
                changed();
            }
            return;
        case Key::Kind::Right:
            if (edit_value_.right()) {
                changed();
            }
            return;
        case Key::Kind::Home:
            if (edit_value_.home()) {
                changed();
            }
            return;
        case Key::Kind::End:
            if (edit_value_.end()) {
                changed();
            }
            return;
        case Key::Kind::Character:
            break;
        default:
            return;
    }

    if (!is_printable(key.character)) {
        return;
    }

    char character = key.character;
    std::size_t limit = kMaxMacro;
    switch (edit_field_) {
        case Field::MyCall:
        case Field::DxCall:
            character = to_upper(character);
            if (!std::isalnum(static_cast<unsigned char>(character)) && character != '/') {
                return;
            }
            limit = kMaxCall;
            break;
        case Field::Device:
            if (character == ' ') {
                return;
            }
            limit = kMaxDevice;
            break;
        case Field::Macro:
            character = to_upper(character);
            break;
        case Field::Serial:
            if (!std::isdigit(static_cast<unsigned char>(character))) {
                return;
            }
            limit = kMaxSerialDigits;
            break;
    }

    if (edit_value_.insert(character, limit)) {
        changed();
    }
}

std::string Terminal::overlay_text() const {
    std::string text;
    switch (overlay_) {
        case Overlay::None:
            break;
        case Overlay::Menu:
            text = menu_edit_armed_ ? "EDIT WHICH MACRO?  1-8, ESC cancels\n"
                                    : "MACROS   1-8 send, E then 1-8 edit\n";
            for (std::size_t index = 0; index < settings_.macros.size(); ++index) {
                text += std::to_string(index + 1) + "  " +
                        preview(settings_.macros[index], kMenuMacroPreview) + "\n";
            }
            text += std::string("M ") + (settings_.line_mode ? "line mode" : "immediate mode") +
                    "    C DX call " + (dx_call_.empty() ? "-" : dx_call_) + "\n";
            text += "N serial " + format_serial(settings_.serial) + "  S setup  D FSK-D  X clear";
            break;
        case Overlay::Setup:
            text = "SETUP\n";
            text += "1  My call   " + settings_.my_call + "\n";
            text += "2  Port      " + settings_.device + "\n";
            text += "3  Baud      " + std::to_string(settings_.baud) + "\n";
            text += std::string("4  Text      ") + (settings_.small_font ? "small" : "normal") + "\n";
            text += std::string("5  Theme     ") + (settings_.dark_mode ? "dark" : "light") + "\n";
            text += "\nPress a number to change, ESC to close.\n";
            text += "Baud must match MENU:RS232 on the KX2.";
            break;
        case Overlay::Edit:
            switch (edit_field_) {
                case Field::MyCall:
                    text = "MY CALL\n";
                    break;
                case Field::DxCall:
                    text = "DX CALL  (fills [CALL] in macros)\n";
                    break;
                case Field::Device:
                    text = "SERIAL PORT\n";
                    break;
                case Field::Macro:
                    text = "MACRO " + std::to_string(edit_macro_ + 1) +
                           "   use [MY] [CALL] [NR]\n";
                    break;
                case Field::Serial:
                    text = "NEXT SERIAL NUMBER  (sent for [NR])\n";
                    break;
            }
            text += "\n" + edit_value_.with_cursor() + "\n\n";
            text += "ENTER save   ESC cancel";
            break;
        case Overlay::Help:
            text = "ZeroRTTY KEYS\n"
                   "ENTER      send line / end the over\n"
                   "TAB        menu: macros, DX call, setup\n"
                   "Fn+1..8    send macro 1-8\n"
                   "Fn+9       set the DX call\n"
                   "Fn+0       line / immediate mode\n"
                   "ESC        stop sending   hold: exit\n"
                   "Fn+F Fn+X  scroll up / down\n"
                   "Fn+Z Fn+C  move the cursor in the line\n"
                   "Fn+H help  Fn+J screenshot   any key closes";
            break;
    }
    return text;
}

} // namespace rtty
