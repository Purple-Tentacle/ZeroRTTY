// SPDX-License-Identifier: MIT

#include "kx2_protocol.h"

#include <cctype>
#include <cstdlib>

namespace rtty {
namespace {

constexpr std::size_t kMaxReplyLength = 64;

bool all_digits(const std::string& value, std::size_t from, std::size_t count) {
    if (from + count > value.size()) {
        return false;
    }
    for (std::size_t index = from; index < from + count; ++index) {
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

} // namespace

void Kx2ReplyParser::feed(const char* data, std::size_t size) {
    buffer_.append(data, size);
}

Kx2Reply Kx2ReplyParser::next() {
    Kx2Reply reply;

    for (;;) {
        // Replies start with two capital letters, or are the "?;" error reply.
        // Anything else is line noise or the tail of a reply we lost the start of.
        while (!buffer_.empty()) {
            const auto first = static_cast<unsigned char>(buffer_[0]);
            if (std::isupper(first) || first == '?') {
                break;
            }
            buffer_.erase(0, 1);
        }
        if (buffer_.size() < 2) {
            return reply;
        }

        if (buffer_.compare(0, 2, "TB") == 0) {
            if (buffer_.size() < 5) {
                return reply;
            }
            if (!all_digits(buffer_, 2, 3)) {
                buffer_.erase(0, 2);
                continue;
            }
            const auto count = static_cast<std::size_t>((buffer_[3] - '0') * 10 + (buffer_[4] - '0'));
            if (buffer_.size() < 5 + count + 1) {
                return reply;
            }
            if (buffer_[5 + count] != ';') {
                // Count and terminator disagree: drop the header and resync.
                buffer_.erase(0, 2);
                continue;
            }
            reply.type = Kx2Reply::Type::Text;
            reply.tx_pending = buffer_[2] - '0';
            reply.text = buffer_.substr(5, count);
            buffer_.erase(0, 5 + count + 1);
            return reply;
        }

        const auto end = buffer_.find(';');
        if (end == std::string::npos) {
            if (buffer_.size() > kMaxReplyLength) {
                buffer_.erase(0, 1);
                continue;
            }
            return reply;
        }

        const std::string token = buffer_.substr(0, end);
        buffer_.erase(0, end + 1);

        if (token.size() == 3 && token.compare(0, 2, "TQ") == 0 && all_digits(token, 2, 1)) {
            reply.type = Kx2Reply::Type::TxState;
            reply.value = token[2] - '0';
        }
        else if (token.size() == 13 && token.compare(0, 2, "FA") == 0 && all_digits(token, 2, 11)) {
            reply.type = Kx2Reply::Type::Frequency;
            reply.frequency_hz = std::atoll(token.c_str() + 2);
        }
        else if (token.size() == 3 && token.compare(0, 2, "MD") == 0 && all_digits(token, 2, 1)) {
            reply.type = Kx2Reply::Type::Mode;
            reply.value = token[2] - '0';
        }
        else if (token.size() == 3 && token.compare(0, 2, "DT") == 0 && all_digits(token, 2, 1)) {
            reply.type = Kx2Reply::Type::DataSubMode;
            reply.value = token[2] - '0';
        }
        else {
            reply.type = Kx2Reply::Type::Other;
        }
        return reply;
    }
}

bool is_sendable(char character, const std::string& extra) {
    const auto value = static_cast<unsigned char>(character);
    if (value < 32 || value > 126) {
        return false;
    }
    // ';' ends the KY command, '<' and '>' switch the radio's TX TEST mode and
    // '@' ends a message, so none of them may ever reach the radio as text.
    if (character == ';' || character == '<' || character == '>' || character == '@') {
        return false;
    }
    if (std::isalnum(value) || character == ' ') {
        return true;
    }
    static const std::string kDefaultPunctuation = ".,?/-";
    return kDefaultPunctuation.find(character) != std::string::npos ||
           extra.find(character) != std::string::npos;
}

std::string sanitize_for_tx(const std::string& text, const std::string& extra) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        if (is_sendable(upper, extra)) {
            result.push_back(upper);
        }
    }
    return result;
}

std::string mode_label(int mode, int data_sub_mode) {
    switch (mode) {
        case 1:
            return "LSB";
        case 2:
            return "USB";
        case 3:
            return "CW";
        case 4:
            return "FM";
        case 5:
            return "AM";
        case 7:
            return "CW-R";
        case 6:
        case 9:
            switch (data_sub_mode) {
                case 0:
                    return "DATA-A";
                case 1:
                    return "AFSK-A";
                case 2:
                    return "FSK-D";
                case 3:
                    return "PSK-D";
                default:
                    return "DATA";
            }
        default:
            return "";
    }
}

bool mode_sends_text(int mode, int data_sub_mode) {
    if (mode == 3 || mode == 7) {
        return true;
    }
    return (mode == 6 || mode == 9) && (data_sub_mode == 2 || data_sub_mode == 3);
}

} // namespace rtty
