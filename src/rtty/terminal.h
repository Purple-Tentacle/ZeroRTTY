// SPDX-License-Identifier: MIT
//
// The behaviour of the ZeroRTTY terminal, without any drawing: what a key does
// in each state, what is queued for the radio and what text is on screen.

#pragma once

#include "kx2_link.h"
#include "settings.h"

#include <functional>
#include <string>

namespace rtty {

struct Key {
    enum class Kind {
        Character,
        Enter,
        Backspace,
        Escape,
        Tab,
        Function, // number = 1..10
        Up,
        Down,
        PageUp,
        PageDown,
        Left,
        Right,
        Home,
        End,
        Delete,
        Help,
    };

    Kind kind{Kind::Character};
    char character{0};
    int number{0};

    static Key of(Kind kind) { return Key{kind, 0, 0}; }
    static Key text(char character) { return Key{Kind::Character, character, 0}; }
    static Key function(int number) { return Key{Kind::Function, 0, number}; }
};

// A line of text with an insertion point, for the input line and the editors.
class LineEdit {
public:
    const std::string& text() const { return text_; }
    std::size_t cursor() const { return cursor_; }
    bool empty() const { return text_.empty(); }

    // Replaces the text and puts the cursor at its end.
    void set(const std::string& text);
    void clear() { set({}); }
    // Inserts at the cursor unless the text already has `limit` characters.
    bool insert(char character, std::size_t limit);
    // Each returns true if something changed.
    bool backspace();
    bool erase();
    bool left();
    bool right();
    bool home();
    bool end();
    // The text with a cursor mark: "_" at the end, "|" inside the text.
    std::string with_cursor() const;

private:
    std::string text_;
    std::size_t cursor_{0};
};

enum class Overlay {
    None,
    Menu,
    Setup,
    Edit,
    Help,
};

struct StatusView {
    enum class Link { NoPort, NoRadio, Ready };

    Link link{Link::NoPort};
    std::string frequency;  // "14.085.00", empty when unknown
    std::string mode;       // "FSK-D", empty when unknown
    bool mode_sends_text{true};
    bool transmitting{false};
    bool line_mode{true};
    std::size_t queued{0};
    std::string dx_call;
    std::string serial;     // "001"; empty unless a macro uses [NR]
    std::string problem;    // text for NoPort / NoRadio
};

class Terminal {
public:
    struct Hooks {
        // Settings changed and should be saved and applied.
        std::function<void()> settings_changed;
    };

    Terminal(Settings& settings, Kx2Link& link, Hooks hooks);

    void handle_key(const Key& key);
    // Collects text and status from the link. Call a few times per second.
    void tick();

    Overlay overlay() const { return overlay_; }
    const std::string& scrollback() const { return scrollback_; }
    const std::string& input_line() const { return input_.text(); }
    std::size_t input_cursor() const { return input_.cursor(); }
    std::string overlay_text() const;
    StatusView status() const;

    // Lines to scroll since the last call (negative = towards older text).
    int take_scroll_lines();
    // True once after the view should jump back to the newest text.
    bool take_follow_request();
    // A short message for the user, empty if there is none.
    std::string take_notice();
    // Changes whenever something visible changed.
    unsigned revision() const { return revision_; }

private:
    enum class Field { MyCall, DxCall, Device, Macro, Serial };
    enum class Direction { None, Received, Sent };

    void handle_main_key(const Key& key);
    void handle_menu_key(const Key& key);
    void handle_setup_key(const Key& key);
    void handle_edit_key(const Key& key);

    // Explains why nothing can be sent right now; true if sending is possible.
    bool ready_to_send();
    void send_macro(std::size_t index);
    void toggle_line_mode();
    bool macros_use_serial() const;
    void begin_edit(Field field, std::size_t macro_index, Overlay return_to);
    void commit_edit();
    void append_scrollback(Direction direction, const std::string& text);
    void notify(const std::string& message);
    void changed() { ++revision_; }
    void settings_changed();
    LinkOptions link_options() const;

    Settings& settings_;
    Kx2Link& link_;
    Hooks hooks_;

    Overlay overlay_{Overlay::None};
    LineEdit input_;
    std::string dx_call_;
    std::string scrollback_;
    Direction last_direction_{Direction::None};
    LinkStatus link_status_;

    // True once the current serial number has gone out in a macro; the next
    // new DX call then moves on to the next number.
    bool serial_used_{false};

    bool menu_edit_armed_{false};
    Field edit_field_{Field::MyCall};
    std::size_t edit_macro_{0};
    Overlay edit_return_{Overlay::None};
    LineEdit edit_value_;

    int scroll_lines_{0};
    bool follow_request_{false};
    std::string notice_;
    unsigned revision_{1};
};

} // namespace rtty
