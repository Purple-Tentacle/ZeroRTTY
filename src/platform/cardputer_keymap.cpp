// SPDX-License-Identifier: MIT

#include "cardputer_keymap.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace platform {
namespace {

struct DefaultEntry {
    uint16_t code;
    char character;
};

// From the CardputerZero launcher (cp0_keyboard_keymap.c).
constexpr DefaultEntry kDefaults[] = {
    {26, '!'}, {27, '@'}, {39, '#'},  {40, '$'}, {41, '%'},  {43, '^'}, {51, '&'}, {52, '*'},
    {53, '('}, {94, ')'}, {55, '~'},  {69, '`'}, {70, '_'},  {71, '-'}, {72, '+'}, {73, '='},
    {74, '['}, {75, ']'}, {76, '{'},  {77, '}'}, {79, ';'},  {80, ':'}, {81, '\''}, {82, '"'},
    {83, '<'}, {85, '>'}, {86, '\\'}, {89, '|'}, {90, ','},  {91, '.'}, {92, '/'}, {93, '?'},
};

struct NamedSymbol {
    const char* name;
    char character;
};

// X keysym names as used in the keymap file.
constexpr NamedSymbol kNames[] = {
    {"exclam", '!'},      {"at", '@'},          {"numbersign", '#'},  {"dollar", '$'},
    {"percent", '%'},     {"asciicircum", '^'}, {"ampersand", '&'},   {"asterisk", '*'},
    {"parenleft", '('},   {"parenright", ')'},  {"asciitilde", '~'},  {"grave", '`'},
    {"underscore", '_'},  {"minus", '-'},       {"plus", '+'},        {"equal", '='},
    {"bracketleft", '['}, {"bracketright", ']'}, {"braceleft", '{'},  {"braceright", '}'},
    {"semicolon", ';'},   {"colon", ':'},       {"apostrophe", '\''}, {"quotedbl", '"'},
    {"less", '<'},        {"greater", '>'},     {"backslash", '\\'},  {"bar", '|'},
    {"comma", ','},       {"period", '.'},      {"slash", '/'},       {"question", '?'},
    {"space", ' '},
};

char character_for_name(const char* name) {
    for (const auto& entry : kNames) {
        if (std::strcmp(entry.name, name) == 0) {
            return entry.character;
        }
    }
    // Letters and digits are named by themselves.
    if (name[0] != '\0' && name[1] == '\0' && name[0] > 32 && name[0] < 127) {
        return name[0];
    }
    return 0;
}

} // namespace

CardputerKeymap::CardputerKeymap() {
    std::memset(table_, 0, sizeof(table_));
    for (const auto& entry : kDefaults) {
        table_[entry.code] = entry.character;
    }
}

int CardputerKeymap::load_text(const std::string& text) {
    char loaded[256];
    std::memset(loaded, 0, sizeof(loaded));
    int count = 0;

    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        unsigned code = 0;
        char name[64] = {0};
        if (std::sscanf(line.c_str(), " keycode %u = %63s", &code, name) != 2) {
            continue;
        }
        const char character = character_for_name(name);
        if (code < sizeof(loaded) && character != 0) {
            loaded[code] = character;
            ++count;
        }
    }

    if (count > 0) {
        std::memcpy(table_, loaded, sizeof(table_));
    }
    return count;
}

int CardputerKeymap::load_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        return 0;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    return load_text(buffer.str());
}

char CardputerKeymap::lookup(uint16_t code) const {
    return code < sizeof(table_) ? table_[code] : 0;
}

} // namespace platform
