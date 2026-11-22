// SPDX-License-Identifier: MIT
//
// Parser for the replies of an Elecraft KX2/KX3 to the handful of CAT commands
// ZeroRTTY uses. Kept free of I/O so it can be tested on its own.

#pragma once

#include <cstddef>
#include <string>

namespace rtty {

struct Kx2Reply {
    enum class Type {
        None,        // nothing complete in the buffer yet
        Text,        // TBtrrs; -> tx_pending, text
        TxState,     // TQn;    -> value (0 = receive, 1 = transmit)
        Frequency,   // FAnnnnnnnnnnn; -> frequency_hz
        Mode,        // MDn;    -> value
        DataSubMode, // DTn;    -> value
        Other,       // any other well-formed reply, ignored
    };

    Type type{Type::None};
    int value{0};
    int tx_pending{0}; // TB: characters still waiting in the radio (0-9)
    long long frequency_hz{0};
    std::string text; // TB: decoded receive text
};

// Takes complete replies off the front of a byte stream. TB replies are
// length-prefixed and may contain ';' in their text, so they cannot be split
// on the terminator alone.
class Kx2ReplyParser {
public:
    void feed(const char* data, std::size_t size);
    Kx2Reply next();
    std::size_t buffered() const { return buffer_.size(); }
    void clear() { buffer_.clear(); }

private:
    std::string buffer_;
};

// Characters that are safe to hand to the radio's KY command for RTTY.
// `extra` adds characters from the configuration. Control characters and the
// KY metacharacters (; < > @) are never allowed.
bool is_sendable(char character, const std::string& extra);

// Upper-cases and drops everything that is not sendable.
std::string sanitize_for_tx(const std::string& text, const std::string& extra);

// Human-readable label for the MD/DT pair, e.g. "FSK-D", "CW", "USB".
std::string mode_label(int mode, int data_sub_mode);

// True for the modes in which the radio turns KY text into a transmission.
bool mode_sends_text(int mode, int data_sub_mode);

} // namespace rtty
