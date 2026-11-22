// SPDX-License-Identifier: MIT

#include "kx2_link.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace rtty {
namespace {

constexpr char kEndOfOver = '\x04';
constexpr int kCycleMs = 120;
constexpr int kTextReplyTimeoutMs = 250;
constexpr int kReopenIntervalMs = 1000;
constexpr int kRadioSilentMs = 2000;
// The radio reports at most 9 waiting characters and one KY command carries at
// most 24. Topping up 10 characters whenever no more than 4 are waiting keeps
// the radio fed without ever getting near its buffer limit.
constexpr int kRefillThreshold = 4;
constexpr std::size_t kChunkSize = 10;
constexpr std::size_t kMaxQueued = 2000;
constexpr std::size_t kMaxBuffered = 8000;

speed_t baud_constant(int baud) {
    switch (baud) {
        case 4800:
            return B4800;
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        default:
            return B0;
    }
}

void append_capped(std::string& target, const std::string& addition) {
    target += addition;
    if (target.size() > kMaxBuffered) {
        target.erase(0, target.size() - kMaxBuffered);
    }
}

} // namespace

Kx2Link::Kx2Link() = default;

Kx2Link::~Kx2Link() {
    stop();
}

bool Kx2Link::is_supported_baud(int baud) {
    return baud_constant(baud) != B0;
}

void Kx2Link::start(const LinkOptions& options) {
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        options_ = options;
        status_ = LinkStatus{};
        reopen_requested_ = false;
    }
    running_ = true;
    worker_ = std::thread(&Kx2Link::run, this);
}

void Kx2Link::stop() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
    close_port();
}

void Kx2Link::reconfigure(const LinkOptions& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool port_changed = options.device != options_.device || options.baud != options_.baud;
    options_ = options;
    reopen_requested_ = reopen_requested_ || port_changed;
}

std::string Kx2Link::send_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Text queued while the radio is away would go on the air whenever it
    // comes back, long after it was typed. Refuse it instead.
    if (!status_.radio_responding) {
        return {};
    }
    std::string accepted = sanitize_for_tx(text, options_.extra_tx_characters);
    if (tx_queue_.size() + accepted.size() > kMaxQueued) {
        accepted.resize(kMaxQueued > tx_queue_.size() ? kMaxQueued - tx_queue_.size() : 0);
    }
    tx_queue_.insert(tx_queue_.end(), accepted.begin(), accepted.end());
    status_.queued = tx_queue_.size();
    return accepted;
}

void Kx2Link::end_over() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.radio_responding) {
        return;
    }
    if (!tx_queue_.empty() && tx_queue_.back() == kEndOfOver) {
        return;
    }
    tx_queue_.push_back(kEndOfOver);
    status_.queued = tx_queue_.size();
}

void Kx2Link::abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    tx_queue_.clear();
    status_.queued = 0;
    abort_requested_ = true;
}

void Kx2Link::select_fsk_d() {
    std::lock_guard<std::mutex> lock(mutex_);
    fsk_d_requested_ = true;
}

std::string Kx2Link::take_received() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string result;
    result.swap(received_);
    return result;
}

std::string Kx2Link::take_sent() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string result;
    result.swap(sent_);
    return result;
}

LinkStatus Kx2Link::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool Kx2Link::open_port() {
    LinkOptions options;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        options = options_;
    }

    std::string error;
    const speed_t speed = baud_constant(options.baud);
    int fd = -1;
    if (speed == B0) {
        error = "unsupported baud rate " + std::to_string(options.baud);
    }
    else {
        fd = ::open(options.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            error = std::strerror(errno);
        }
    }

    if (fd >= 0) {
        termios settings{};
        if (tcgetattr(fd, &settings) != 0) {
            error = std::string("not a serial port: ") + std::strerror(errno);
            ::close(fd);
            fd = -1;
        }
        else {
            cfmakeraw(&settings);
            settings.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
            settings.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS | CSTOPB | PARENB);
            settings.c_cc[VMIN] = 0;
            settings.c_cc[VTIME] = 0;
            cfsetispeed(&settings, speed);
            cfsetospeed(&settings, speed);
            if (tcsetattr(fd, TCSANOW, &settings) != 0) {
                error = std::string("could not configure port: ") + std::strerror(errno);
                ::close(fd);
                fd = -1;
            }
            else {
                tcflush(fd, TCIOFLUSH);
            }
        }
    }

    fd_ = fd;
    parser_.clear();
    awaiting_text_reply_ = false;
    chunk_unconfirmed_ = false;
    last_received_was_cr_ = false;
    last_reply_ = Clock::time_point{};

    std::lock_guard<std::mutex> lock(mutex_);
    status_.port_open = fd >= 0;
    status_.port_error = error;
    status_.radio_responding = false;
    status_.transmitting = false;
    status_.radio_pending = 0;
    return fd >= 0;
}

void Kx2Link::close_port() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status_.port_open = false;
    status_.radio_responding = false;
    status_.transmitting = false;
    tx_queue_.clear();
    status_.queued = 0;
}

bool Kx2Link::write_all(const std::string& data) {
    std::size_t written = 0;
    int stalls = 0;
    while (written < data.size()) {
        const auto result = ::write(fd_, data.data() + written, data.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EINTR) && ++stalls < 50) {
            pollfd descriptor{fd_, POLLOUT, 0};
            ::poll(&descriptor, 1, 20);
            continue;
        }
        return false;
    }
    return true;
}

void Kx2Link::handle_reply(const Kx2Reply& reply) {
    last_reply_ = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    switch (reply.type) {
        case Kx2Reply::Type::Text: {
            awaiting_text_reply_ = false;
            status_.radio_pending = reply.tx_pending;
            std::string cleaned;
            for (const char character : reply.text) {
                if (character == '\r' || character == '\n') {
                    if (!(character == '\n' && last_received_was_cr_)) {
                        cleaned.push_back('\n');
                    }
                    last_received_was_cr_ = character == '\r';
                    continue;
                }
                last_received_was_cr_ = false;
                const auto value = static_cast<unsigned char>(character);
                if (value >= 32 && value <= 126) {
                    cleaned.push_back(character);
                }
            }
            append_capped(received_, cleaned);
            break;
        }
        case Kx2Reply::Type::TxState:
            status_.transmitting = reply.value != 0;
            break;
        case Kx2Reply::Type::Frequency:
            status_.frequency_hz = reply.frequency_hz;
            break;
        case Kx2Reply::Type::Mode:
            status_.mode = reply.value;
            break;
        case Kx2Reply::Type::DataSubMode:
            status_.data_sub_mode = reply.value;
            break;
        default:
            break;
    }
}

void Kx2Link::read_available(int timeout_ms) {
    if (fd_ < 0) {
        return;
    }

    pollfd descriptor{fd_, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    if (ready <= 0) {
        return;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
        (descriptor.revents & POLLIN) == 0) {
        close_port();
        return;
    }

    char buffer[256];
    for (;;) {
        const auto count = ::read(fd_, buffer, sizeof(buffer));
        if (count > 0) {
            parser_.feed(buffer, static_cast<std::size_t>(count));
            if (count < static_cast<ssize_t>(sizeof(buffer))) {
                break;
            }
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            break;
        }
        if (count < 0) {
            // The adapter was unplugged.
            close_port();
            return;
        }
        break;
    }

    for (;;) {
        const auto reply = parser_.next();
        if (reply.type == Kx2Reply::Type::None) {
            break;
        }
        handle_reply(reply);
    }
}

void Kx2Link::pump_transmit() {
    std::string chunk;
    std::string echoed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tx_queue_.empty() || chunk_unconfirmed_ || !status_.radio_responding ||
            status_.radio_pending > kRefillThreshold) {
            return;
        }
        // Text sent in a voice mode would be lost, so it waits until the radio
        // is in a mode that transmits it. An unknown mode is given the benefit
        // of the doubt.
        if (status_.mode != 0 && !mode_sends_text(status_.mode, status_.data_sub_mode)) {
            return;
        }

        const bool data_mode = status_.mode == 6 || status_.mode == 9;
        while (!tx_queue_.empty() && echoed.size() < kChunkSize) {
            const char character = tx_queue_.front();
            tx_queue_.pop_front();
            if (character == kEndOfOver) {
                // Only the direct data modes need to be told that the over has
                // ended; CW simply stops.
                if (data_mode) {
                    chunk.push_back(kEndOfOver);
                }
                break;
            }
            chunk.push_back(character);
            echoed.push_back(character);
        }
        status_.queued = tx_queue_.size();
    }

    if (chunk.empty()) {
        return;
    }
    if (!write_all("KY " + chunk + ";")) {
        close_port();
        return;
    }
    chunk_unconfirmed_ = true;

    std::lock_guard<std::mutex> lock(mutex_);
    append_capped(sent_, echoed);
}

void Kx2Link::run() {
    unsigned cycle = 0;
    bool text_request_after_chunk = false;

    while (running_) {
        bool reopen = false;
        bool abort_now = false;
        bool select_fsk_d_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reopen = reopen_requested_;
            reopen_requested_ = false;
            abort_now = abort_requested_;
            abort_requested_ = false;
            select_fsk_d_now = fsk_d_requested_;
            fsk_d_requested_ = false;
        }

        if (reopen) {
            close_port();
            last_open_attempt_ = Clock::time_point{};
        }

        if (fd_ < 0) {
            const auto now = Clock::now();
            if (now - last_open_attempt_ >= std::chrono::milliseconds(kReopenIntervalMs)) {
                last_open_attempt_ = now;
                if (open_port()) {
                    cycle = 0;
                    text_request_after_chunk = false;
                }
            }
            if (fd_ < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        std::string commands;
        if (abort_now) {
            commands += "RX;";
            chunk_unconfirmed_ = false;
            text_request_after_chunk = false;
        }
        if (select_fsk_d_now) {
            commands += "MD6;DT2;";
        }
        commands += "TB;";
        if (cycle % 2 == 0) {
            commands += "TQ;";
        }
        if (cycle % 8 == 0) {
            commands += "FA;MD;DT;";
        }
        ++cycle;

        if (!write_all(commands)) {
            close_port();
            continue;
        }
        awaiting_text_reply_ = true;
        if (chunk_unconfirmed_) {
            text_request_after_chunk = true;
        }

        const auto cycle_start = Clock::now();
        const auto elapsed_ms = [&cycle_start]() {
            return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        Clock::now() - cycle_start)
                                        .count());
        };

        while (fd_ >= 0 && awaiting_text_reply_ && elapsed_ms() < kTextReplyTimeoutMs) {
            read_available(20);
        }
        if (fd_ < 0) {
            continue;
        }

        if (!awaiting_text_reply_ && chunk_unconfirmed_ && text_request_after_chunk) {
            // The radio has reported its buffer level since the last chunk.
            chunk_unconfirmed_ = false;
            text_request_after_chunk = false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.radio_responding =
                last_reply_ != Clock::time_point{} &&
                Clock::now() - last_reply_ < std::chrono::milliseconds(kRadioSilentMs);
            if (!status_.radio_responding) {
                status_.transmitting = false;
                tx_queue_.clear();
                status_.queued = 0;
            }
        }

        pump_transmit();

        while (fd_ >= 0 && elapsed_ms() < kCycleMs) {
            read_available(kCycleMs - elapsed_ms());
        }
    }
}

} // namespace rtty
