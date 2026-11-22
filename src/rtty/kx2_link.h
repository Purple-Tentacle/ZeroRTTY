// SPDX-License-Identifier: MIT
//
// Serial link to an Elecraft KX2 (KXUSB cable on the ACC jack). A worker thread
// polls the radio for decoded text and status and feeds queued text to the
// radio's KY command in small chunks.

#pragma once

#include "kx2_protocol.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace rtty {

struct LinkOptions {
    std::string device{"/dev/ttyUSB0"};
    int baud{38400};
    std::string extra_tx_characters;
};

struct LinkStatus {
    bool port_open{false};
    bool radio_responding{false};
    bool transmitting{false};
    long long frequency_hz{0};
    int mode{0};
    int data_sub_mode{-1};
    std::size_t queued{0};     // characters waiting in ZeroRTTY
    int radio_pending{0};      // characters waiting in the radio (0-9)
    std::string port_error;    // why the port could not be opened
};

class Kx2Link {
public:
    Kx2Link();
    ~Kx2Link();

    Kx2Link(const Kx2Link&) = delete;
    Kx2Link& operator=(const Kx2Link&) = delete;

    void start(const LinkOptions& options);
    void stop();
    // Applies a new device/baud; the port is reopened by the worker.
    void reconfigure(const LinkOptions& options);

    // Queues text for transmission. Returns the text as it will be sent
    // (upper-cased, unsendable characters removed). Nothing is queued, and an
    // empty string is returned, while the radio is not answering; whatever is
    // still queued when the radio goes away is dropped.
    std::string send_text(const std::string& text);
    // Queues the end of the over: the radio returns to receive once the text
    // queued before it has been sent.
    void end_over();
    // Drops everything queued and tells the radio to stop transmitting now.
    void abort();
    // Switches the radio to DATA / FSK D.
    void select_fsk_d();

    // Decoded receive text since the last call.
    std::string take_received();
    // Text handed to the radio since the last call.
    std::string take_sent();
    LinkStatus status() const;

    static bool is_supported_baud(int baud);

private:
    using Clock = std::chrono::steady_clock;

    void run();
    bool open_port();
    void close_port();
    bool write_all(const std::string& data);
    void read_available(int timeout_ms);
    void handle_reply(const Kx2Reply& reply);
    void pump_transmit();

    mutable std::mutex mutex_;
    LinkOptions options_;
    bool reopen_requested_{false};
    std::deque<char> tx_queue_; // '\x04' marks the end of an over
    bool abort_requested_{false};
    bool fsk_d_requested_{false};
    std::string received_;
    std::string sent_;
    LinkStatus status_;

    std::thread worker_;
    std::atomic<bool> running_{false};

    // Worker-only state.
    int fd_{-1};
    Kx2ReplyParser parser_;
    bool awaiting_text_reply_{false};
    bool chunk_unconfirmed_{false};
    bool last_received_was_cr_{false};
    Clock::time_point last_reply_{};
    Clock::time_point last_open_attempt_{};
};

} // namespace rtty
