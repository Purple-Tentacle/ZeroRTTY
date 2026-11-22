/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "asset_manager.h"
#include "base_viewmodel.h"
#include "kx2_link.h"
#include "linux_input.h"
#include "logger.h"
#include "screenshot_service.h"
#include "settings.h"
#include "terminal.h"
#include "terminal_screen.h"
#include "theme.h"
#include "toast.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#if USE_DESKTOP
#include "desktop_simulator_frame.h"
#endif

#if !USE_DESKTOP
#if APP_USE_DRM
#include "src/drivers/display/drm/lv_linux_drm.h"
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#endif

#ifndef APP_FRAMEBUFFER_DEVICE
#define APP_FRAMEBUFFER_DEVICE "/dev/fb0"
#endif

#ifndef APP_DRM_DEVICE
#define APP_DRM_DEVICE "/dev/dri/card0"
#endif

#ifndef APP_DRM_CONNECTOR_ID
#define APP_DRM_CONNECTOR_ID -1
#endif

#ifndef APP_CONFIG_FILE
#define APP_CONFIG_FILE "zerortty.conf"
#endif

namespace app {
namespace {

// SDL reports F1..F12 as (1 << 30) | scancode, scancodes 58..69. The desktop
// simulator passes these through for Fn + digit.
constexpr uint32_t kSdlF1 = 0x4000003AU;
constexpr uint32_t kSdlF10 = 0x40000043U;

constexpr uint32_t kRefreshMs = 50;

void quit_requested_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* running = static_cast<bool*>(lv_observer_get_user_data(observer));
    if (running && lv_subject_get_int(subject)) {
        *running = false;
    }
}

std::string writable_config_path() {
    if (const char* override_path = std::getenv("ZERORTTY_CONFIG")) {
        if (override_path[0] != '\0') {
            return override_path;
        }
    }
#if USE_DESKTOP
    return APP_CONFIG_FILE;
#else
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME")) {
        const std::filesystem::path root(xdg_config_home);
        if (!root.empty() && root.is_absolute()) {
            return (root / "zerortty" / "zerortty.conf").string();
        }
    }
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path root(home);
        if (!root.empty() && root.is_absolute()) {
            return (root / ".config" / "zerortty" / "zerortty.conf").string();
        }
    }
    return APP_CONFIG_FILE;
#endif
}

rtty::LinkOptions link_options(const rtty::Settings& settings) {
    rtty::LinkOptions options;
    options.device = settings.device;
    options.baud = settings.baud;
    options.extra_tx_characters = settings.extra_tx_characters;
    return options;
}

// Connects the keyboard, the terminal logic, the radio link and the screen.
class TerminalController {
public:
    TerminalController(lv_display_t* display,
                       viewmodel::BaseViewModel& view_model,
                       AssetManager& assets,
                       rtty::Settings& settings,
                       std::string config_path)
        : display_(display),
          view_model_(view_model),
          settings_(settings),
          config_path_(std::move(config_path)),
          terminal_(settings_, link_, {[this]() { on_settings_changed(); }}),
          screen_(assets),
          toast_(std::make_unique<view::widgets::Toast>(lv_layer_top(), view_model, assets)) {
        toast_->build();
        lv_screen_load(screen_.root());
        link_.start(link_options(settings_));

        platform::set_global_key_listener(global_key_listener, this);
        timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
        refresh(true);
    }

    ~TerminalController() {
        if (timer_) {
            lv_timer_delete(timer_);
        }
        platform::clear_global_key_listener(global_key_listener, this);
        link_.stop();
    }

    // Keys from a test script; see README "Testing without a radio".
    void inject(const rtty::Key& key) {
        terminal_.handle_key(key);
        refresh(false);
    }

private:
    void on_settings_changed() {
        std::string error;
        if (!rtty::save_settings(config_path_, settings_, error)) {
            LOG_WARN("failed to save config {}: {}", config_path_, error);
            toast_->show("Settings not saved: " + error, view::widgets::ToastTone::Error, 3200);
        }
        if (view_model_.is_dark_mode() != settings_.dark_mode) {
            view_model_.set_dark_mode(settings_.dark_mode);
            view::apply_lvgl_theme(display_, settings_.dark_mode, LV_FONT_DEFAULT);
        }
    }

    void refresh(bool force) {
        terminal_.tick();
        const auto notice = terminal_.take_notice();
        if (!notice.empty()) {
            LOG_INFO("notice: {}", notice);
            toast_->show(notice, view::widgets::ToastTone::Default, 2200);
        }
        if (force || terminal_.revision() != shown_revision_) {
            shown_revision_ = terminal_.revision();
            screen_.render(terminal_, settings_.dark_mode, settings_.small_font);
        }
    }

    bool handle_key(uint32_t key, bool long_pressed) {
        if (long_pressed) {
            if (key == LV_KEY_ESC && terminal_.overlay() == rtty::Overlay::None) {
                toast_->hide();
                view_model_.request_quit();
            }
            return true;
        }

        if (key == platform::kKeyPrintScreen) {
            const auto result = platform::screenshot::capture_active_screen();
            if (result.success) {
                toast_->show("Saved " + result.path, view::widgets::ToastTone::Success, 4000);
            }
            else {
                toast_->show("Screenshot failed: " + result.error,
                             view::widgets::ToastTone::Error,
                             3200);
            }
            return true;
        }

        using Kind = rtty::Key::Kind;
        rtty::Key translated;
        bool known = true;
        if (key >= platform::kKeyFunctionBase + 1U && key <= platform::kKeyFunctionBase + 10U) {
            translated = rtty::Key::function(static_cast<int>(key - platform::kKeyFunctionBase));
        }
        else if (key >= kSdlF1 && key <= kSdlF10) {
            translated = rtty::Key::function(static_cast<int>(key - kSdlF1) + 1);
        }
        else {
            switch (key) {
                case LV_KEY_ENTER:
                case '\r':
                    translated = rtty::Key::of(Kind::Enter);
                    break;
                case LV_KEY_BACKSPACE:
                case LV_KEY_DEL:
                    translated = rtty::Key::of(Kind::Backspace);
                    break;
                case LV_KEY_ESC:
                    translated = rtty::Key::of(Kind::Escape);
                    break;
                case platform::kKeyTab:
                case LV_KEY_NEXT:
                    translated = rtty::Key::of(Kind::Tab);
                    break;
                case LV_KEY_UP:
                    translated = rtty::Key::of(Kind::Up);
                    break;
                case LV_KEY_DOWN:
                    translated = rtty::Key::of(Kind::Down);
                    break;
                case platform::kKeyPageUp:
                case LV_KEY_PREV:
                    translated = rtty::Key::of(Kind::PageUp);
                    break;
                case platform::kKeyPageDown:
                    translated = rtty::Key::of(Kind::PageDown);
                    break;
                case LV_KEY_END:
                    translated = rtty::Key::of(Kind::End);
                    break;
                case platform::kKeyHelp:
                    translated = rtty::Key::of(Kind::Help);
                    break;
                default:
                    if (key >= 32 && key <= 126) {
                        translated = rtty::Key::text(static_cast<char>(key));
                    }
                    else {
                        known = false;
                    }
                    break;
            }
        }

        if (known) {
            terminal_.handle_key(translated);
            refresh(false);
        }
        // Every key belongs to the terminal; nothing falls through to the
        // template's navigation shortcuts.
        return true;
    }

    static bool global_key_listener(uint32_t key, const char*, bool long_pressed, void* user_data) {
        auto* controller = static_cast<TerminalController*>(user_data);
        return controller && controller->handle_key(key, long_pressed);
    }

    static void timer_cb(lv_timer_t* timer) {
        auto* controller = static_cast<TerminalController*>(lv_timer_get_user_data(timer));
        if (controller) {
            controller->refresh(false);
        }
    }

    lv_display_t* display_;
    viewmodel::BaseViewModel& view_model_;
    rtty::Settings& settings_;
    std::string config_path_;
    rtty::Kx2Link link_;
    rtty::Terminal terminal_;
    screen::TerminalScreen screen_;
    std::unique_ptr<view::widgets::Toast> toast_;
    lv_timer_t* timer_{nullptr};
    unsigned shown_revision_{0};
};

// Test hook: ZERORTTY_TEST_KEYS types a script after start-up, one key every
// 120 ms. "\n" Enter, "\t" Tab, "\e" Esc, "\b" Backspace, "\h" Help,
// "\1".."\9" and "\0" Fn+digit, "\u" / "\d" scroll, "\w" waits one second.
class KeyScript {
public:
    KeyScript(TerminalController& controller, const char* script)
        : controller_(controller), script_(script ? script : "") {
        if (!script_.empty()) {
            timer_ = lv_timer_create(timer_cb, 120, this);
        }
    }

    ~KeyScript() {
        if (timer_) {
            lv_timer_delete(timer_);
        }
    }

private:
    void step() {
        if (wait_ticks_ > 0) {
            --wait_ticks_;
            return;
        }
        if (position_ >= script_.size()) {
            lv_timer_delete(timer_);
            timer_ = nullptr;
            return;
        }

        using Kind = rtty::Key::Kind;
        char character = script_[position_++];
        if (character != '\\' || position_ >= script_.size()) {
            controller_.inject(rtty::Key::text(character));
            return;
        }
        character = script_[position_++];
        switch (character) {
            case 'n':
                controller_.inject(rtty::Key::of(Kind::Enter));
                break;
            case 't':
                controller_.inject(rtty::Key::of(Kind::Tab));
                break;
            case 'e':
                controller_.inject(rtty::Key::of(Kind::Escape));
                break;
            case 'b':
                controller_.inject(rtty::Key::of(Kind::Backspace));
                break;
            case 'h':
                controller_.inject(rtty::Key::of(Kind::Help));
                break;
            case 'w':
                wait_ticks_ = 8;
                break;
            case 'u':
                controller_.inject(rtty::Key::of(Kind::Up));
                break;
            case 'd':
                controller_.inject(rtty::Key::of(Kind::Down));
                break;
            case '0':
                controller_.inject(rtty::Key::function(10));
                break;
            default:
                if (character >= '1' && character <= '9') {
                    controller_.inject(rtty::Key::function(character - '0'));
                }
                break;
        }
    }

    static void timer_cb(lv_timer_t* timer) {
        static_cast<KeyScript*>(lv_timer_get_user_data(timer))->step();
    }

    TerminalController& controller_;
    std::string script_;
    std::size_t position_{0};
    int wait_ticks_{0};
    lv_timer_t* timer_{nullptr};
};

#if !USE_DESKTOP
lv_display_t* init_device_display() {
#if APP_USE_DRM
    auto* display = lv_linux_drm_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_drm_set_file(display, APP_DRM_DEVICE, APP_DRM_CONNECTOR_ID) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#else
    auto* display = lv_linux_fbdev_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_fbdev_set_file(display, APP_FRAMEBUFFER_DEVICE) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#endif
}
#endif

} // namespace

int Application::run() {
    logger::Logger::init();
    logger::Logger::set_tag("zerortty");

    lv_init();

    AssetManager assets;
    for (const auto& root : assets.roots()) {
        LOG_INFO("asset root: {}", root.string());
    }

    // The packaged defaults come first, the user's own file is layered on top.
    viewmodel::BaseViewModel view_model;
    const std::string user_config_path = writable_config_path();
    rtty::Settings settings;
    std::string config_error;
    if (!rtty::load_settings(APP_CONFIG_FILE, settings, config_error)) {
        LOG_WARN("failed to load config {}: {}; using defaults", APP_CONFIG_FILE, config_error);
    }
    if (user_config_path != APP_CONFIG_FILE) {
        std::error_code filesystem_error;
        if (std::filesystem::is_regular_file(user_config_path, filesystem_error) &&
            !rtty::load_settings(user_config_path, settings, config_error)) {
            LOG_WARN("failed to load config {}: {}", user_config_path, config_error);
        }
    }
    if (const char* port = std::getenv("ZERORTTY_PORT")) {
        if (port[0] != '\0') {
            settings.device = port;
        }
    }
    if (!rtty::Kx2Link::is_supported_baud(settings.baud)) {
        LOG_WARN("unsupported baud rate {} in config; using 38400", settings.baud);
        settings.baud = 38400;
    }
    view_model.set_dark_mode(settings.dark_mode);
    LOG_INFO("port {} at {} baud, call {}", settings.device, settings.baud, settings.my_call);

#if USE_DESKTOP
    DesktopSimulatorFrame simulator_frame(assets);
    auto* display = simulator_frame.display();
#else
    auto* display = init_device_display();
#endif
    if (!display) {
        LOG_ERROR("failed to initialize display");
        return 1;
    }

    view::apply_lvgl_theme(display, view_model.is_dark_mode(), LV_FONT_DEFAULT);

#if USE_DESKTOP
    simulator_frame.bind_dark_mode(view_model.dark_mode_subject());
#endif

    TerminalController controller(display, view_model, assets, settings, user_config_path);
    KeyScript key_script(controller, std::getenv("ZERORTTY_TEST_KEYS"));

    bool running = true;
    auto* quit_observer = lv_subject_add_observer(view_model.quit_requested_subject(),
                                                  quit_requested_observer,
                                                  &running);

    // Test hook: save a screenshot after the given number of milliseconds and exit.
    uint32_t screenshot_after_ms = 0;
    if (const char* value = std::getenv("ZERORTTY_SCREENSHOT_AFTER")) {
        screenshot_after_ms = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    }
    const uint32_t started_at = lv_tick_get();

    LOG_INFO("LVGL app started at {}x{}", lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));
    while (running
#if USE_DESKTOP
           && simulator_frame.process_events()
#endif
    ) {
        lv_timer_handler();
        lv_delay_ms(5);
        if (screenshot_after_ms != 0 && lv_tick_elaps(started_at) >= screenshot_after_ms) {
            const auto result = platform::screenshot::capture_active_screen();
            if (result.success) {
                LOG_INFO("screenshot saved: {}", result.path);
            }
            else {
                LOG_WARN("screenshot failed: {}", result.error);
            }
            running = false;
        }
    }

    if (quit_observer) {
        lv_observer_remove(quit_observer);
    }

    return 0;
}

} // namespace app
