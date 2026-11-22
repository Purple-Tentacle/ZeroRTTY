// SPDX-License-Identifier: MIT
//
// A pretend KX2 for trying ZeroRTTY without a radio. Prints the name of a
// pseudo terminal to use as the port and "receives" a line of RTTY now and then.
//
//   zerortty_kx2sim &
//   ZERORTTY_PORT=/dev/pts/N ./zerortty

#include "kx2_sim.h"

#include <cstdio>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 0;
    rtty_test::Kx2Sim sim(6);
    std::printf("%s\n", sim.path().c_str());
    std::fflush(stdout);

    const char* lines[] = {
        "CQ CQ CQ DE W1AW W1AW W1AW K\r\n",
        "RYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRY\r\n",
        "DA2IT DE W1AW UR 599 599 NAME HIRAM QTH NEWINGTON CT BK\r\n",
    };
    std::size_t shown = 0;
    std::size_t next_line = 0;
    for (int elapsed = 0; seconds == 0 || elapsed < seconds * 10; ++elapsed) {
        if (elapsed % 60 == 5) {
            sim.receive(lines[next_line++ % 3]);
        }
        const auto transmitted = sim.transmitted();
        if (transmitted.size() > shown) {
            std::fprintf(stderr, "%s", transmitted.substr(shown).c_str());
            shown = transmitted.size();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}
