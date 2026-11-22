// SPDX-License-Identifier: MIT
//
// A stand-in for a KX2 on a pseudo terminal: enough of the CAT protocol for
// ZeroRTTY's tests and for trying the app without a radio.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace rtty_test {

class Kx2Sim {
public:
    // The real radio's KY buffer size is not documented; one command may carry
    // 24 characters, so the simulator fails anything that needs more room.
    static constexpr std::size_t kBufferCapacity = 24;

    explicit Kx2Sim(int characters_per_second = 60) : rate_(characters_per_second) {
        master_ = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (master_ < 0 || grantpt(master_) != 0 || unlockpt(master_) != 0) {
            std::perror("posix_openpt");
            std::exit(2);
        }
        path_ = ptsname(master_);
        // Keep one descriptor open on the slave side so the master does not see
        // a hangup while the application has the port closed.
        keep_open_ = ::open(path_.c_str(), O_RDWR | O_NOCTTY);
        running_ = true;
        worker_ = std::thread(&Kx2Sim::run, this);
    }

    ~Kx2Sim() { stop(); }

    void stop() {
        running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
        if (keep_open_ >= 0) {
            ::close(keep_open_);
            keep_open_ = -1;
        }
        if (master_ >= 0) {
            ::close(master_);
            master_ = -1;
        }
    }

    const std::string& path() const { return path_; }

    void receive(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        rx_pending_ += text;
    }

    void set_mode(int mode, int data_sub_mode) {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = mode;
        data_sub_mode_ = data_sub_mode;
    }

    void set_silent(bool silent) { silent_ = silent; }

    std::string transmitted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return transmitted_;
    }
    int end_of_over_count() const { return end_of_over_count_; }
    int abort_count() const { return abort_count_; }
    int overflow_count() const { return overflow_count_; }
    std::size_t longest_command() const { return longest_command_; }
    std::size_t peak_buffer() const { return peak_buffer_; }
    int mode() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mode_;
    }
    int data_sub_mode() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_sub_mode_;
    }
    std::vector<std::string> unknown_commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return unknown_;
    }

private:
    using Clock = std::chrono::steady_clock;

    void reply(const std::string& text) {
        if (silent_) {
            return;
        }
        std::size_t written = 0;
        while (written < text.size()) {
            const auto count = ::write(master_, text.data() + written, text.size() - written);
            if (count > 0) {
                written += static_cast<std::size_t>(count);
            }
            else {
                break;
            }
        }
    }

    void handle(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        longest_command_ = std::max(longest_command_.load(), command.size());
        if (command == "TB") {
            const auto count = std::min<std::size_t>(rx_pending_.size(), 40);
            char header[8];
            std::snprintf(header,
                          sizeof(header),
                          "TB%d%02d",
                          static_cast<int>(std::min<std::size_t>(buffer_.size(), 9)),
                          static_cast<int>(count));
            reply(header + rx_pending_.substr(0, count) + ";");
            rx_pending_.erase(0, count);
        }
        else if (command == "TQ") {
            reply(transmitting_ ? "TQ1;" : "TQ0;");
        }
        else if (command == "FA") {
            reply("FA00014085000;");
        }
        else if (command == "MD") {
            reply("MD" + std::to_string(mode_) + ";");
        }
        else if (command == "DT") {
            reply("DT" + std::to_string(data_sub_mode_) + ";");
        }
        else if (command == "MD6") {
            mode_ = 6;
        }
        else if (command == "DT2") {
            data_sub_mode_ = 2;
        }
        else if (command == "RX") {
            buffer_.clear();
            transmitting_ = false;
            ++abort_count_;
        }
        else if (command.compare(0, 3, "KY ") == 0) {
            const auto text = command.substr(3);
            if (text.size() > 24 || buffer_.size() + text.size() > kBufferCapacity) {
                ++overflow_count_;
            }
            buffer_ += text;
            peak_buffer_ = std::max(peak_buffer_.load(), buffer_.size());
            transmitting_ = true;
        }
        else {
            unknown_.push_back(command);
            reply("?;");
        }
    }

    void run() {
        std::string pending;
        auto next_character = Clock::now();
        while (running_) {
            pollfd descriptor{master_, POLLIN, 0};
            ::poll(&descriptor, 1, 5);
            char bytes[256];
            const auto count = ::read(master_, bytes, sizeof(bytes));
            if (count > 0) {
                pending.append(bytes, static_cast<std::size_t>(count));
                std::size_t end = 0;
                while ((end = pending.find(';')) != std::string::npos) {
                    handle(pending.substr(0, end));
                    pending.erase(0, end + 1);
                }
            }

            const auto now = Clock::now();
            if (now >= next_character) {
                next_character = now + std::chrono::milliseconds(1000 / rate_);
                std::lock_guard<std::mutex> lock(mutex_);
                if (!buffer_.empty()) {
                    const char character = buffer_.front();
                    buffer_.erase(0, 1);
                    if (character == '\x04') {
                        ++end_of_over_count_;
                        transmitting_ = false;
                    }
                    else {
                        transmitted_.push_back(character);
                    }
                }
            }
        }
    }

    int rate_;
    int master_{-1};
    int keep_open_{-1};
    std::string path_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> silent_{false};
    std::atomic<int> end_of_over_count_{0};
    std::atomic<int> abort_count_{0};
    std::atomic<int> overflow_count_{0};
    std::atomic<std::size_t> longest_command_{0};
    std::atomic<std::size_t> peak_buffer_{0};

    mutable std::mutex mutex_;
    std::string buffer_;
    std::string transmitted_;
    std::string rx_pending_;
    std::vector<std::string> unknown_;
    bool transmitting_{false};
    int mode_{6};
    int data_sub_mode_{2};
};

} // namespace rtty_test
