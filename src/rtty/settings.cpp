// SPDX-License-Identifier: MIT

#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace rtty {
namespace {

std::string trim(std::string value) {
    const auto is_not_space = [](unsigned char character) { return !std::isspace(character); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), is_not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(), value.end());
    return value;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool parse_bool(const std::string& value, bool& result) {
    const auto normalized = lowercase(trim(value));
    if (normalized == "yes" || normalized == "true" || normalized == "1") {
        result = true;
        return true;
    }
    if (normalized == "no" || normalized == "false" || normalized == "0") {
        result = false;
        return true;
    }
    return false;
}

const char* yes_no(bool value) {
    return value ? "yes" : "no";
}

} // namespace

bool load_settings(const std::string& path, Settings& settings, std::string& error) {
    error.clear();
    std::ifstream input(path);
    if (!input) {
        error = "could not open file";
        return false;
    }

    std::string section;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']' && line.find('=') == std::string::npos) {
            section = lowercase(trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const auto key = lowercase(trim(line.substr(0, separator)));
        const auto value = trim(line.substr(separator + 1));
        const auto fail = [&](const std::string& name) {
            error = "invalid " + name + " value at line " + std::to_string(line_number);
            return false;
        };

        if (section == "application") {
            if (key == "dark_mode" && !parse_bool(value, settings.dark_mode)) return fail(key);
            if (key == "small_font" && !parse_bool(value, settings.small_font)) return fail(key);
            if (key == "line_mode" && !parse_bool(value, settings.line_mode)) return fail(key);
        }
        else if (section == "station") {
            if (key == "my_call") settings.my_call = value;
            if (key == "serial") {
                char* end = nullptr;
                const long serial = std::strtol(value.c_str(), &end, 10);
                if (value.empty() || *end != '\0' || serial < 1 || serial > 99999) return fail(key);
                settings.serial = static_cast<int>(serial);
            }
        }
        else if (section == "radio") {
            if (key == "device") {
                if (value.empty()) return fail(key);
                settings.device = value;
            }
            else if (key == "baud") {
                char* end = nullptr;
                const long baud = std::strtol(value.c_str(), &end, 10);
                if (value.empty() || *end != '\0' || baud <= 0) return fail(key);
                settings.baud = static_cast<int>(baud);
            }
            else if (key == "extra_tx_characters") {
                settings.extra_tx_characters = value;
            }
        }
        else if (section == "macros") {
            if (key.size() == 1 && key[0] >= '1' && key[0] < static_cast<char>('1' + kMacroCount)) {
                settings.macros[static_cast<std::size_t>(key[0] - '1')] = value;
            }
        }
    }
    return true;
}

bool save_settings(const std::string& path, const Settings& settings, std::string& error) {
    error.clear();

    const std::filesystem::path config_path(path);
    const auto parent = config_path.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            error = "could not create parent directory: " + filesystem_error.message();
            return false;
        }
    }

    // Write to a temporary file first so a full disk or a power cut cannot
    // leave a truncated configuration behind.
    const std::string temporary_path = path + ".tmp";
    {
        std::ofstream output(temporary_path, std::ios::trunc);
        if (!output) {
            error = "could not open file for writing";
            return false;
        }
        output << "# ZeroRTTY settings. Changes made in the app are written here.\n"
               << "[application]\n"
               << "dark_mode=" << yes_no(settings.dark_mode) << '\n'
               << "small_font=" << yes_no(settings.small_font) << '\n'
               << "line_mode=" << yes_no(settings.line_mode) << '\n'
               << "\n[station]\n"
               << "my_call=" << settings.my_call << '\n'
               << "serial=" << settings.serial << '\n'
               << "\n[radio]\n"
               << "device=" << settings.device << '\n'
               << "baud=" << settings.baud << '\n'
               << "extra_tx_characters=" << settings.extra_tx_characters << '\n'
               << "\n[macros]\n";
        for (std::size_t index = 0; index < settings.macros.size(); ++index) {
            output << (index + 1) << '=' << settings.macros[index] << '\n';
        }
        output.flush();
        if (!output) {
            error = "failed while writing file";
            return false;
        }
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary_path, config_path, rename_error);
    if (rename_error) {
        error = "could not replace file: " + rename_error.message();
        return false;
    }
    return true;
}

} // namespace rtty
