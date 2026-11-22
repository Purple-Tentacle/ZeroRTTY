// SPDX-License-Identifier: MIT
//
// Talks to the KX2 without the UI. Useful over SSH to check the cable, the
// baud rate and the radio's mode before blaming the app.
//
//   zerortty_cli status
//   zerortty_cli monitor [seconds]
//   zerortty_cli send "CQ CQ DE DA2IT K"      (this transmits)

#include "kx2_link.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

void usage() {
    std::puts("usage: zerortty_cli [--port DEVICE] [--baud RATE] COMMAND\n"
              "  status            show what the radio reports\n"
              "  monitor [SECONDS] print decoded text (default 30 s)\n"
              "  send TEXT         transmit TEXT, then return to receive\n"
              "defaults: --port /dev/ttyUSB0 --baud 38400");
}

bool wait_for_radio(rtty::Kx2Link& link) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        const auto status = link.status();
        // The frequency, mode and data sub-mode replies arrive one after the
        // other. In DATA mode the sub-mode decides whether text can be sent,
        // so do not report before it is known.
        const bool data_mode = status.mode == 6 || status.mode == 9;
        if (status.radio_responding && status.mode != 0 && status.frequency_hz != 0 &&
            (!data_mode || status.data_sub_mode >= 0)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

void print_status(const rtty::LinkStatus& status) {
    std::printf("frequency  %lld Hz\n", status.frequency_hz);
    std::printf("mode       %s%s\n",
                rtty::mode_label(status.mode, status.data_sub_mode).c_str(),
                rtty::mode_sends_text(status.mode, status.data_sub_mode)
                    ? ""
                    : "  (text is not transmitted in this mode)");
    std::printf("state      %s\n", status.transmitting ? "transmit" : "receive");
}

} // namespace

int main(int argc, char** argv) {
    rtty::LinkOptions options;
    int index = 1;
    while (index + 1 < argc && std::strncmp(argv[index], "--", 2) == 0) {
        if (std::strcmp(argv[index], "--port") == 0) {
            options.device = argv[index + 1];
        }
        else if (std::strcmp(argv[index], "--baud") == 0) {
            options.baud = std::atoi(argv[index + 1]);
        }
        else {
            usage();
            return 2;
        }
        index += 2;
    }
    if (index >= argc) {
        usage();
        return 2;
    }
    const std::string command = argv[index++];
    if (command != "status" && command != "monitor" && command != "send") {
        usage();
        return 2;
    }
    if (command == "send" && index >= argc) {
        usage();
        return 2;
    }

    rtty::Kx2Link link;
    link.start(options);
    if (!wait_for_radio(link)) {
        const auto status = link.status();
        if (!status.port_open) {
            std::fprintf(stderr,
                         "cannot open %s: %s\n",
                         options.device.c_str(),
                         status.port_error.empty() ? "unknown error" : status.port_error.c_str());
        }
        else {
            std::fprintf(stderr,
                         "%s is open but the radio does not answer at %d baud.\n"
                         "Check that the KX2 is on and that MENU:RS232 matches.\n",
                         options.device.c_str(),
                         options.baud);
        }
        return 1;
    }

    if (command == "status") {
        print_status(link.status());
        return 0;
    }

    if (command == "monitor") {
        const int seconds = index < argc ? std::atoi(argv[index]) : 30;
        print_status(link.status());
        std::printf("--- decoded text for %d s (TEXT DECODE must be on at the radio) ---\n", seconds);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto text = link.take_received();
            if (!text.empty()) {
                std::fputs(text.c_str(), stdout);
                std::fflush(stdout);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::puts("");
        return 0;
    }

    const auto status = link.status();
    if (!rtty::mode_sends_text(status.mode, status.data_sub_mode)) {
        std::fprintf(stderr,
                     "the radio is in %s; select FSK D, PSK D or CW first\n",
                     rtty::mode_label(status.mode, status.data_sub_mode).c_str());
        return 1;
    }
    const auto accepted = link.send_text(argv[index]);
    if (accepted.empty()) {
        std::fprintf(stderr, "nothing sendable in the text\n");
        return 1;
    }
    link.end_over();
    std::printf("sending: %s\n", accepted.c_str());

    // Up to a second passes before the first characters reach the radio, so
    // wait for the queue to drain rather than for the transmit flag alone.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = link.status();
        if (!now.port_open || !now.radio_responding) {
            std::fprintf(stderr, "lost the radio while sending\n");
            return 1;
        }
        if (now.queued == 0 && now.radio_pending == 0 && !now.transmitting) {
            std::puts("done");
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    link.abort();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    std::fprintf(stderr, "still transmitting after 120 s; stopped\n");
    return 1;
}
