// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace rtty {

constexpr std::size_t kMacroCount = 8;

struct Settings {
    bool dark_mode{false};
    bool small_font{false};
    bool line_mode{true};
    std::string my_call{"NOCALL"};
    std::string device{"/dev/ttyUSB0"};
    int baud{38400};
    // Punctuation to allow in transmitted text beyond ". , ? / -".
    std::string extra_tx_characters;
    std::array<std::string, kMacroCount> macros{{
        "CQ CQ CQ DE [MY] [MY] [MY] K",
        "CQ POTA CQ POTA DE [MY] [MY] K",
        "[CALL] DE [MY] 599 599 K",
        "[CALL] TU 73 DE [MY] SK",
        "QRZ DE [MY] K",
        "[CALL] AGN AGN K",
        "[MY]",
        "",
    }};
};

// Reads the keys present in `path` into `settings`; keys that are missing keep
// their current value, so a per-user file can be layered over the system one.
bool load_settings(const std::string& path, Settings& settings, std::string& error);

// Writes all settings to `path`, creating the directory if needed.
bool save_settings(const std::string& path, const Settings& settings, std::string& error);

} // namespace rtty
