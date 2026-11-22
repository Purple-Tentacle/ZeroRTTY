// SPDX-License-Identifier: MIT
//
// Tests for the parts of ZeroRTTY that do not draw: the reply parser, the
// serial link (against a simulated KX2), the terminal behaviour and settings.

#include "cardputer_keymap.h"
#include "kx2_link.h"
#include "kx2_protocol.h"
#include "kx2_sim.h"
#include "settings.h"
#include "terminal.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        ++checks;                                                                     \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);          \
        }                                                                             \
    } while (false)

#define CHECK_EQ(actual, expected)                                                    \
    do {                                                                              \
        ++checks;                                                                     \
        const auto actual_value = (actual);                                           \
        const auto expected_value = (expected);                                       \
        if (!(actual_value == expected_value)) {                                      \
            ++failures;                                                               \
            std::printf("FAIL %s:%d  %s\n   got:      [%s]\n   expected: [%s]\n",    \
                        __FILE__, __LINE__, #actual,                                  \
                        std::string(actual_value).c_str(),                            \
                        std::string(expected_value).c_str());                         \
        }                                                                             \
    } while (false)

bool wait_for(const std::function<bool()>& condition, int timeout_ms = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

rtty::LinkOptions options_for(const rtty_test::Kx2Sim& sim) {
    rtty::LinkOptions options;
    options.device = sim.path();
    options.baud = 38400;
    return options;
}

void type(rtty::Terminal& terminal, const std::string& text) {
    for (const char character : text) {
        terminal.handle_key(rtty::Key::text(character));
    }
}

void test_parser() {
    rtty::Kx2ReplyParser parser;

    // The reply the user's radio gave with nothing decoded.
    parser.feed("TB000;", 6);
    auto reply = parser.next();
    CHECK(reply.type == rtty::Kx2Reply::Type::Text);
    CHECK_EQ(reply.text, std::string());
    CHECK(reply.tx_pending == 0);
    CHECK(parser.next().type == rtty::Kx2Reply::Type::None);

    // Receive text may contain the terminator; the count decides.
    const std::string with_semicolon = "TB307AB;CD;E;TQ1;";
    parser.feed(with_semicolon.data(), with_semicolon.size());
    reply = parser.next();
    CHECK(reply.type == rtty::Kx2Reply::Type::Text);
    CHECK_EQ(reply.text, std::string("AB;CD;E"));
    CHECK(reply.tx_pending == 3);
    reply = parser.next();
    CHECK(reply.type == rtty::Kx2Reply::Type::TxState);
    CHECK(reply.value == 1);

    // A reply split across reads is completed by the next read.
    parser.feed("TB005HE", 7);
    CHECK(parser.next().type == rtty::Kx2Reply::Type::None);
    parser.feed("LLO;", 4);
    reply = parser.next();
    CHECK_EQ(reply.text, std::string("HELLO"));

    // The frequency reply as seen on the user's radio.
    parser.feed("FA00028491000;MD6;DT2;", 22);
    reply = parser.next();
    CHECK(reply.type == rtty::Kx2Reply::Type::Frequency);
    CHECK(reply.frequency_hz == 28491000);
    reply = parser.next();
    CHECK(reply.type == rtty::Kx2Reply::Type::Mode);
    CHECK(reply.value == 6);
    reply = parser.next();
    CHECK(reply.type == rtty::Kx2Reply::Type::DataSubMode);
    CHECK(reply.value == 2);

    // The tail of a reply whose start was lost must not block what follows.
    parser.feed("28491000;TQ0;", 13);
    reply = parser.next();
    CHECK(reply.type == rtty::Kx2Reply::Type::TxState);
    CHECK(reply.value == 0);

    // Unknown and error replies are skipped.
    parser.feed("?;XY12;TQ1;", 11);
    CHECK(parser.next().type == rtty::Kx2Reply::Type::Other);
    CHECK(parser.next().type == rtty::Kx2Reply::Type::Other);
    CHECK(parser.next().type == rtty::Kx2Reply::Type::TxState);

    // A TB header whose count does not match the data is dropped.
    parser.clear();
    parser.feed("TB003ABCDEF;TQ1;", 16);
    bool saw_tx_state = false;
    for (int index = 0; index < 6; ++index) {
        reply = parser.next();
        if (reply.type == rtty::Kx2Reply::Type::Text) {
            CHECK(false);
        }
        if (reply.type == rtty::Kx2Reply::Type::TxState) {
            saw_tx_state = true;
        }
    }
    CHECK(saw_tx_state);
}

void test_sanitize() {
    CHECK_EQ(rtty::sanitize_for_tx("cq de da2it k", ""), std::string("CQ DE DA2IT K"));
    // KY metacharacters and control characters never get through.
    CHECK_EQ(rtty::sanitize_for_tx("A;B<C>D@E\x04" "F\n", ";<>@"), std::string("ABCDEF"));
    CHECK_EQ(rtty::sanitize_for_tx("599 5NN? W1AW/P -.,", ""), std::string("599 5NN? W1AW/P -.,"));
    CHECK_EQ(rtty::sanitize_for_tx("A(B)C", ""), std::string("ABC"));
    CHECK_EQ(rtty::sanitize_for_tx("A(B)C", "()"), std::string("A(B)C"));
    CHECK_EQ(rtty::mode_label(6, 2), std::string("FSK-D"));
    CHECK_EQ(rtty::mode_label(3, 0), std::string("CW"));
    CHECK(rtty::mode_sends_text(6, 2));
    CHECK(rtty::mode_sends_text(3, -1));
    CHECK(!rtty::mode_sends_text(2, -1));
    CHECK(!rtty::mode_sends_text(6, 0));
}

void test_link_transmit() {
    rtty_test::Kx2Sim sim(60);
    rtty::Kx2Link link;
    link.start(options_for(sim));
    CHECK(wait_for([&] { return link.status().radio_responding; }));
    CHECK(link.status().port_open);
    CHECK(wait_for([&] { return link.status().frequency_hz == 14085000; }));
    CHECK(wait_for([&] { return link.status().mode == 6 && link.status().data_sub_mode == 2; }));

    std::string text;
    for (int index = 0; index < 6; ++index) {
        text += "CQ CQ CQ DE DA2IT DA2IT K ";
    }
    CHECK_EQ(link.send_text(text), text);
    link.end_over();

    CHECK(wait_for([&] { return sim.end_of_over_count() == 1; }, 15000));
    CHECK_EQ(sim.transmitted(), text);
    CHECK(sim.overflow_count() == 0);
    CHECK(sim.longest_command() <= 3 + 24);
    CHECK(sim.peak_buffer() <= rtty_test::Kx2Sim::kBufferCapacity);
    CHECK(sim.unknown_commands().empty());
    CHECK(wait_for([&] { return link.status().queued == 0 && !link.status().transmitting; }));
    CHECK_EQ(link.take_sent(), text);
    link.stop();
}

void test_link_slow_radio_never_overflows() {
    // 45 baud RTTY is about 6 characters per second.
    rtty_test::Kx2Sim sim(6);
    rtty::Kx2Link link;
    link.start(options_for(sim));
    CHECK(wait_for([&] { return link.status().radio_responding; }));

    const std::string text = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789";
    link.send_text(text);
    link.end_over();
    CHECK(wait_for([&] { return sim.end_of_over_count() == 1; }, 20000));
    CHECK_EQ(sim.transmitted(), text);
    CHECK(sim.overflow_count() == 0);
    link.stop();
}

void test_link_receive_and_abort() {
    rtty_test::Kx2Sim sim(6);
    rtty::Kx2Link link;
    link.start(options_for(sim));
    CHECK(wait_for([&] { return link.status().radio_responding; }));

    const std::string heard =
        "CQ CQ DE W1AW W1AW; PSE K\r\nRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRY\x07 END";
    sim.receive(heard);
    std::string received;
    CHECK(wait_for([&] {
        received += link.take_received();
        return received.size() >= heard.size() - 2;
    }));
    CHECK_EQ(received,
             std::string("CQ CQ DE W1AW W1AW; PSE K\n"
                         "RYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRY END"));

    // Stopping drops what is queued and tells the radio to stop.
    link.send_text(std::string(200, 'X'));
    CHECK(wait_for([&] { return sim.transmitted().size() >= 3; }));
    link.abort();
    CHECK(wait_for([&] { return sim.abort_count() == 1; }));
    CHECK(link.status().queued == 0);
    const auto sent_at_abort = sim.transmitted().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(sim.transmitted().size() <= sent_at_abort + 1);
    CHECK(sim.transmitted().size() < 60);

    // The link still works afterwards.
    link.take_sent();
    link.send_text("OK");
    link.end_over();
    CHECK(wait_for([&] { return sim.end_of_over_count() == 1; }));
    CHECK_EQ(link.take_sent(), std::string("OK"));
    link.stop();
}

void test_link_mode_gating_and_fsk_d() {
    rtty_test::Kx2Sim sim(60);
    sim.set_mode(2, 0); // USB
    rtty::Kx2Link link;
    link.start(options_for(sim));
    CHECK(wait_for([&] { return link.status().mode == 2; }));

    // Text waits while the radio is in a voice mode instead of being lost.
    link.send_text("HELD");
    link.end_over();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK_EQ(sim.transmitted(), std::string());
    CHECK(link.status().queued == 5);

    link.select_fsk_d();
    CHECK(wait_for([&] { return sim.mode() == 6 && sim.data_sub_mode() == 2; }));
    CHECK(wait_for([&] { return sim.end_of_over_count() == 1; }));
    CHECK_EQ(sim.transmitted(), std::string("HELD"));
    link.stop();
}

void test_link_cw_has_no_end_marker() {
    rtty_test::Kx2Sim sim(60);
    sim.set_mode(3, 0); // CW
    rtty::Kx2Link link;
    link.start(options_for(sim));
    CHECK(wait_for([&] { return link.status().mode == 3; }));
    link.send_text("TEST");
    link.end_over();
    CHECK(wait_for([&] { return sim.transmitted() == "TEST"; }));
    CHECK(wait_for([&] { return link.status().queued == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(sim.end_of_over_count() == 0);
    link.stop();
}

void test_link_problems() {
    // A port that does not exist is reported, not fatal.
    rtty::Kx2Link missing;
    rtty::LinkOptions options;
    options.device = "/dev/zerortty-does-not-exist";
    missing.start(options);
    CHECK(wait_for([&] { return !missing.status().port_error.empty(); }));
    CHECK(!missing.status().port_open);

    // Nothing is accepted for sending while there is no radio: it would
    // otherwise be transmitted whenever the radio turns up.
    CHECK_EQ(missing.send_text("QUEUED"), std::string());
    CHECK(missing.status().queued == 0);

    // Pointing it at a working port recovers without a restart.
    rtty_test::Kx2Sim sim(6);
    missing.reconfigure(options_for(sim));
    CHECK(wait_for([&] { return missing.status().radio_responding; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK_EQ(sim.transmitted(), std::string());

    // Text still queued when the radio goes quiet is dropped, not kept for later.
    missing.send_text(std::string(100, 'Q'));
    CHECK(wait_for([&] { return sim.transmitted().size() >= 2; }));
    CHECK(missing.status().queued > 0);

    // A radio that stops answering is noticed, and so is its return.
    sim.set_silent(true);
    CHECK(wait_for([&] { return !missing.status().radio_responding; }, 5000));
    CHECK(missing.status().port_open);
    CHECK(missing.status().queued == 0);
    CHECK_EQ(missing.send_text("LATE"), std::string());
    sim.set_silent(false);
    CHECK(wait_for([&] { return missing.status().radio_responding; }));
    missing.stop();

    // An unsupported baud rate is refused with a reason.
    rtty::Kx2Link bad_baud;
    auto bad = options_for(sim);
    bad.baud = 1234;
    bad_baud.start(bad);
    CHECK(wait_for([&] { return !bad_baud.status().port_error.empty(); }));
    CHECK(!bad_baud.status().port_open);
    bad_baud.stop();
}

void test_terminal() {
    rtty_test::Kx2Sim sim(60);
    rtty::Settings settings;
    settings.my_call = "DA2IT";
    settings.device = sim.path();
    rtty::Kx2Link link;
    int saves = 0;
    rtty::Terminal terminal(settings, link, {[&] { ++saves; }});
    rtty::LinkOptions options;
    options.device = settings.device;
    link.start(options);
    CHECK(wait_for([&] {
        terminal.tick();
        return terminal.status().link == rtty::StatusView::Link::Ready;
    }));
    CHECK_EQ(terminal.status().frequency, std::string("14.085.00"));
    CHECK_EQ(terminal.status().mode, std::string("FSK-D"));

    // In a voice mode the line is kept and the user is told why.
    sim.set_mode(2, 0);
    CHECK(wait_for([&] {
        terminal.tick();
        return terminal.status().mode == "USB";
    }));
    CHECK(!terminal.status().mode_sends_text);
    type(terminal, "keep me");
    terminal.take_notice();
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK_EQ(terminal.input_line(), std::string("KEEP ME"));
    CHECK(terminal.take_notice().find("USB") != std::string::npos);
    terminal.handle_key(rtty::Key::function(1));
    CHECK(!terminal.take_notice().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK_EQ(sim.transmitted(), std::string());
    CHECK(link.status().queued == 0);
    for (int index = 0; index < 7; ++index) {
        terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Backspace));
    }
    sim.set_mode(6, 2);
    CHECK(wait_for([&] {
        terminal.tick();
        return terminal.status().mode == "FSK-D";
    }));

    // Line mode: nothing is sent until ENTER, and backspace corrects.
    type(terminal, "hello wx");
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Backspace));
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Backspace));
    type(terminal, "all");
    CHECK_EQ(terminal.input_line(), std::string("HELLO ALL"));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK_EQ(sim.transmitted(), std::string());
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK_EQ(terminal.input_line(), std::string());
    CHECK(wait_for([&] { return sim.end_of_over_count() == 1; }));
    CHECK_EQ(sim.transmitted(), std::string("HELLO ALL "));

    // Characters RTTY cannot carry are refused with a message.
    terminal.take_notice();
    type(terminal, "<");
    CHECK_EQ(terminal.input_line(), std::string());
    CHECK(!terminal.take_notice().empty());

    // A macro that needs the DX call is not sent without one.
    terminal.handle_key(rtty::Key::function(3));
    CHECK(!terminal.take_notice().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(sim.transmitted(), std::string("HELLO ALL "));

    // Fn+9 sets the DX call; then the macro goes out with both calls filled in.
    terminal.handle_key(rtty::Key::function(9));
    CHECK(terminal.overlay() == rtty::Overlay::Edit);
    type(terminal, "w1aw!");
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK(terminal.overlay() == rtty::Overlay::None);
    CHECK_EQ(terminal.status().dx_call, std::string("W1AW"));
    terminal.handle_key(rtty::Key::function(3));
    CHECK(wait_for([&] { return sim.end_of_over_count() == 2; }));
    CHECK_EQ(sim.transmitted(), std::string("HELLO ALL W1AW DE DA2IT 599 599 K "));

    // Contest serial: [NR] is the current number, three digits. It stays the
    // same for repeats and moves on when a new DX call is entered afterwards.
    CHECK_EQ(settings.macros[7], std::string("[CALL] 599 [NR] [NR]"));
    CHECK_EQ(terminal.status().serial, std::string("001"));
    terminal.handle_key(rtty::Key::function(8));
    terminal.handle_key(rtty::Key::function(8));
    CHECK(wait_for([&] { return sim.end_of_over_count() == 4; }));
    CHECK(sim.transmitted().find("W1AW 599 001 001 W1AW 599 001 001 ") != std::string::npos);
    CHECK(settings.serial == 1);
    // Re-entering the same call is a correction, not a new QSO.
    terminal.handle_key(rtty::Key::function(9));
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK(settings.serial == 1);
    terminal.take_notice();
    terminal.handle_key(rtty::Key::function(9));
    for (int index = 0; index < 4; ++index) {
        terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Backspace));
    }
    type(terminal, "dl1abc");
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK(settings.serial == 2);
    CHECK(terminal.take_notice().find("002") != std::string::npos);
    CHECK_EQ(terminal.status().serial, std::string("002"));
    // A second new call without the number having gone out does not skip one.
    terminal.handle_key(rtty::Key::function(9));
    type(terminal, "x");
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK(settings.serial == 2);
    terminal.handle_key(rtty::Key::function(8));
    CHECK(wait_for([&] { return sim.end_of_over_count() == 5; }));
    CHECK(sim.transmitted().find("DL1ABCX 599 002 002 ") != std::string::npos);
    // The number can be set by hand from the menu; only digits are taken.
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Tab));
    type(terminal, "n");
    CHECK(terminal.overlay() == rtty::Overlay::Edit);
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Backspace));
    type(terminal, "1x234");
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK(terminal.overlay() == rtty::Overlay::Menu);
    CHECK(settings.serial == 1234);
    CHECK_EQ(terminal.status().serial, std::string("1234"));
    // Zero is refused and the editor stays open.
    type(terminal, "n");
    for (int index = 0; index < 4; ++index) {
        terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Backspace));
    }
    type(terminal, "0");
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK(terminal.overlay() == rtty::Overlay::Edit);
    CHECK(settings.serial == 1234);
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Escape));
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Escape));
    CHECK(terminal.overlay() == rtty::Overlay::None);
    // Back to the state the following checks expect.
    terminal.handle_key(rtty::Key::function(9));
    for (int index = 0; index < 7; ++index) {
        terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Backspace));
    }
    type(terminal, "w1aw");
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    const std::string before_macro_edit = sim.transmitted();
    const int overs_before_macro_edit = sim.end_of_over_count();

    // Menu: E then 8 edits macro 8, and the change is saved.
    const int saves_before = saves;
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Tab));
    CHECK(terminal.overlay() == rtty::Overlay::Menu);
    type(terminal, "e8");
    CHECK(terminal.overlay() == rtty::Overlay::Edit);
    for (int index = 0; index < 30; ++index) {
        terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Backspace));
    }
    type(terminal, "tu [my] test");
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK(terminal.overlay() == rtty::Overlay::Menu);
    CHECK_EQ(settings.macros[7], std::string("TU [MY] TEST"));
    CHECK(saves == saves_before + 1);

    // Menu: a digit sends that macro and closes the menu.
    type(terminal, "8");
    CHECK(terminal.overlay() == rtty::Overlay::None);
    CHECK(wait_for([&] { return sim.end_of_over_count() == overs_before_macro_edit + 1; }));
    CHECK_EQ(sim.transmitted(), before_macro_edit + "TU DA2IT TEST ");

    // Cancelling an edit keeps the old value.
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Tab));
    type(terminal, "e1");
    type(terminal, "XXX");
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Escape));
    CHECK_EQ(settings.macros[0], std::string("CQ CQ CQ DE [MY] [MY] [MY] K"));
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Escape));
    CHECK(terminal.overlay() == rtty::Overlay::None);

    // Immediate mode: each key goes out at once, ENTER ends the over.
    terminal.handle_key(rtty::Key::function(10));
    CHECK(!settings.line_mode);
    type(terminal, "ab");
    CHECK(wait_for([&] {
        return sim.transmitted().size() >= 2 &&
               sim.transmitted().substr(sim.transmitted().size() - 2) == "AB";
    }));
    CHECK_EQ(terminal.input_line(), std::string());
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK(wait_for([&] { return sim.end_of_over_count() == overs_before_macro_edit + 2; }));
    terminal.handle_key(rtty::Key::function(10));
    CHECK(settings.line_mode);

    // Received and sent text end up on separate lines of the scrollback.
    sim.receive("DA2IT DE W1AW 599");
    CHECK(wait_for([&] {
        terminal.tick();
        return terminal.scrollback().find("DA2IT DE W1AW 599") != std::string::npos;
    }));
    const auto& scrollback = terminal.scrollback();
    CHECK(scrollback.find("> HELLO ALL W1AW DE DA2IT 599 599 K") == 0);
    CHECK(scrollback.find("\nDA2IT DE W1AW 599") != std::string::npos);

    // Setup: baud cycles through the rates the KX2 offers and wraps.
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Tab));
    type(terminal, "s");
    CHECK(terminal.overlay() == rtty::Overlay::Setup);
    type(terminal, "3");
    CHECK(settings.baud == 4800);
    type(terminal, "333");
    CHECK(settings.baud == 38400);
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Escape));

    // Without a radio the typed line stays put and nothing is queued.
    sim.set_silent(true);
    CHECK(wait_for([&] {
        terminal.tick();
        return terminal.status().link == rtty::StatusView::Link::NoRadio;
    }, 5000));
    type(terminal, "anyone");
    terminal.take_notice();
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK_EQ(terminal.input_line(), std::string("ANYONE"));
    CHECK(terminal.take_notice().find("not connected") != std::string::npos);
    CHECK(link.status().queued == 0);
    sim.set_silent(false);
    CHECK(wait_for([&] {
        terminal.tick();
        return terminal.status().link == rtty::StatusView::Link::Ready;
    }));
    for (int index = 0; index < 6; ++index) {
        terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Backspace));
    }

    // ESC stops a transmission in progress.
    type(terminal, std::string(120, 'z'));
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Enter));
    CHECK(wait_for([&] {
        terminal.tick();
        return terminal.status().queued > 0 || terminal.status().transmitting;
    }));
    terminal.handle_key(rtty::Key::of(rtty::Key::Kind::Escape));
    CHECK(wait_for([&] { return sim.abort_count() == 1; }));
    link.stop();
}

void test_cardputer_keymap() {
    // Built-in table: the Sym layer of the CardputerZero keyboard.
    platform::CardputerKeymap keymap;
    CHECK(keymap.lookup(90) == ',');
    CHECK(keymap.lookup(91) == '.');
    CHECK(keymap.lookup(92) == '/');
    CHECK(keymap.lookup(93) == '?');
    CHECK(keymap.lookup(71) == '-');
    CHECK(keymap.lookup(74) == '[');
    CHECK(keymap.lookup(75) == ']');
    // 51 is KEY_COMMA on a PC keyboard but '&' here.
    CHECK(keymap.lookup(51) == '&');
    // Letters, digits and unknown codes are left to the ordinary mapping.
    CHECK(keymap.lookup(30) == 0);
    CHECK(keymap.lookup(2) == 0);
    CHECK(keymap.lookup(300) == 0);

    // A keymap file in the image replaces the table.
    const int count = keymap.load_text("# comment\n"
                                       "keycode 90 = period   # swapped\n"
                                       "keycode 91 = comma\n"
                                       "  keycode 100 = question\n"
                                       "keycode 101 = NoSuchName\n"
                                       "garbage line\n");
    CHECK(count == 3);
    CHECK(keymap.lookup(90) == '.');
    CHECK(keymap.lookup(91) == ',');
    CHECK(keymap.lookup(100) == '?');
    CHECK(keymap.lookup(101) == 0);
    CHECK(keymap.lookup(92) == 0);

    // A file without usable lines keeps what was there.
    CHECK(keymap.load_text("nothing here\n") == 0);
    CHECK(keymap.lookup(90) == '.');
    CHECK(keymap.load_file("/nonexistent/keymap.map") == 0);
}

void test_settings() {
    const auto directory = std::filesystem::temp_directory_path() / "zerortty-test";
    std::filesystem::remove_all(directory);
    const auto path = (directory / "nested" / "zerortty.conf").string();

    rtty::Settings written;
    written.dark_mode = true;
    written.line_mode = false;
    written.my_call = "DA2IT/P";
    written.device = "/dev/ttyUSB1";
    written.baud = 9600;
    written.macros[2] = "[CALL] DE [MY] = 599";
    written.macros[6] = "";
    written.serial = 137;
    std::string error;
    CHECK(rtty::save_settings(path, written, error));

    rtty::Settings read;
    CHECK(rtty::load_settings(path, read, error));
    CHECK(read.dark_mode);
    CHECK(!read.line_mode);
    CHECK_EQ(read.my_call, std::string("DA2IT/P"));
    CHECK_EQ(read.device, std::string("/dev/ttyUSB1"));
    CHECK(read.baud == 9600);
    CHECK_EQ(read.macros[2], std::string("[CALL] DE [MY] = 599"));
    CHECK_EQ(read.macros[6], std::string());
    CHECK(read.serial == 137);
    CHECK_EQ(read.macros[0], std::string("CQ CQ CQ DE [MY] [MY] [MY] K"));

    // A file with only some keys leaves the rest alone.
    const auto partial = (directory / "partial.conf").string();
    {
        FILE* file = std::fopen(partial.c_str(), "w");
        std::fputs("[station]\nmy_call = W1AW\n[macros]\n7=[MY]\n", file);
        std::fclose(file);
    }
    CHECK(rtty::load_settings(partial, read, error));
    CHECK_EQ(read.my_call, std::string("W1AW"));
    CHECK_EQ(read.macros[6], std::string("[MY]"));
    CHECK(read.baud == 9600);

    // A broken value is reported instead of being guessed.
    const auto broken = (directory / "broken.conf").string();
    {
        FILE* file = std::fopen(broken.c_str(), "w");
        std::fputs("[radio]\nbaud=fast\n", file);
        std::fclose(file);
    }
    CHECK(!rtty::load_settings(broken, read, error));
    CHECK(!error.empty());
    CHECK(!rtty::load_settings((directory / "missing.conf").string(), read, error));
    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    test_parser();
    test_sanitize();
    test_cardputer_keymap();
    test_settings();
    test_link_transmit();
    test_link_slow_radio_never_overflows();
    test_link_receive_and_abort();
    test_link_mode_gating_and_fsk_d();
    test_link_cw_has_no_end_marker();
    test_link_problems();
    test_terminal();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
