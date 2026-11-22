// SPDX-License-Identifier: MIT
//
// The CardputerZero's built-in keyboard (a TCA8418 keypad) reports the symbols
// of its Sym layer as key codes of their own: Sym+V arrives as code 90, not as
// a modifier plus KEY_V. Those codes overlap ordinary Linux key codes with a
// different meaning, so they must only be applied to the built-in keyboard.

#pragma once

#include <cstdint>
#include <string>

namespace platform {

// The image's own keymap file, also read by the launcher. When it exists it
// replaces the built-in table, so a changed keyboard layout is picked up.
constexpr const char* kCardputerKeymapPath = "/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map";

class CardputerKeymap {
public:
    // Starts with the table built into the launcher.
    CardputerKeymap();

    // Replaces the table with "keycode N = name" lines. Returns the number of
    // entries read; the built-in table is kept if there are none.
    int load_text(const std::string& text);
    int load_file(const std::string& path);

    // The character for a Sym-layer key code, or 0 if it is not one.
    char lookup(uint16_t code) const;

private:
    char table_[256];
};

} // namespace platform
