// PS5Droid - PS5 payload entry point (native Orbis homebrew).
//
// STATUS: implemented against API usage verified in ps5-payload-dev/sdk
// (sce_stubs + samples) and ps5-payload-dev/SDL, but NEVER EXECUTED ON A
// CONSOLE. It is compiled only when the ps5-payload-sdk toolchain drives
// the build (PS5DROID_PLATFORM_PS5). Until a console run is documented,
// this binary must be treated as experimental and untested.
//
// What it does:
//   1. hides the splash screen,
//   2. initializes a 1920x1080 double-buffered video-out surface and the
//      DualSense pad,
//   3. runs the platform capability probes and drives a pad-navigable
//      launcher with probe / status / matrix / about screens (all content
//      built by the portable ui/screens module),
//   4. exits cleanly (releasing direct memory) on Exit/Options.
//
// v1.0.2 note: all six launcher actions now have real screens; the Linux
// ABI matrix screen is paged with L1/R1. Screen content is produced by
// ui/screens (host-tested); this file only renders and routes input.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "android/runtime_manager.h"
#include "compat/linux_abi.h"
#include "compat/memory_compat.h"
#include "compat/syscall_dispatch.h"
#include "platform/display.h"
#include "platform/platform.h"
#include "platform/ps5/sce_decls.h"
#include "ui/bitmap_font.h"
#include "ui/launcher.h"
#include "ui/screens.h"
#include "version.h"

using namespace ps5droid;

namespace {

const platform::Rgba kBackground{16, 18, 26, 255};
const platform::Rgba kTitleColor{90, 200, 250, 255};
const platform::Rgba kTextColor{220, 225, 235, 255};
const platform::Rgba kSelectColor{255, 210, 80, 255};

constexpr int kMaxLines = 14;

enum class ScreenId { kMenu = 0, kProbes, kStatus, kMatrix, kAbout };

void draw_lines(platform::Framebuffer* fb, const std::vector<ui::ScreenLine>& lines) {
    int y = 240;
    int count = static_cast<int>(lines.size());
    if (count > kMaxLines)
        count = kMaxLines;
    for (int i = 0; i < count; ++i) {
        const platform::Rgba& color =
            lines[static_cast<size_t>(i)].highlight ? kSelectColor : kTextColor;
        ui::render_text(fb, 200, y, lines[static_cast<size_t>(i)].text, color, 2);
        y += 56;
    }
}

void show_screen(platform::Framebuffer* fb, const char* title,
                 const std::vector<ui::ScreenLine>& lines) {
    fb->clear(kBackground);
    ui::render_text_centered(fb, 40, title, kTitleColor, 4);
    ui::render_text_centered(fb, 140, "NATIVE PS5 RESEARCH RUNTIME - EXPERIMENTAL", kTextColor, 2);
    draw_lines(fb, lines);
    ui::render_text(fb, 200, 1020,
                    "DPAD: NAVIGATE  CROSS: SELECT  CIRCLE: BACK  "
                    "OPTIONS: EXIT",
                    kTextColor, 2);
}

void show_menu(platform::Framebuffer* fb, const ui::LauncherModel& launcher) {
    std::vector<ui::ScreenLine> lines;
    for (const auto& item : launcher.items())
        lines.emplace_back(item.label, false);
    fb->clear(kBackground);
    ui::render_text_centered(fb, 40, "PS5DROID", kTitleColor, 4);
    std::string subtitle = "V";
    subtitle += ps5droid::kVersion;
    subtitle += " - NATIVE PS5 RESEARCH RUNTIME";
    ui::render_text_centered(fb, 140, subtitle, kTextColor, 2);

    int y = 240;
    int selected = launcher.selected();
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        bool is_sel = (i == selected);
        ui::render_text(fb, 200, y, is_sel ? ">" : " ", is_sel ? kSelectColor : kTextColor, 2);
        ui::render_text(fb, 240, y, lines[static_cast<size_t>(i)].text,
                        is_sel ? kSelectColor : kTextColor, 2);
        y += 56;
    }
    ui::render_text(fb, 200, 1020,
                    "DPAD: NAVIGATE  CROSS: SELECT  CIRCLE: BACK  "
                    "OPTIONS: EXIT",
                    kTextColor, 2);
}

void run_launcher(platform::DisplayBackend* display, platform::InputBackend* input) {
    ui::LauncherModel launcher;
    android::RuntimeManager manager;
    android::CapabilityReport report = manager.probe();
    // Honest start attempt once at boot: its structured blockers are what
    // the status screen reports (the state machine cannot reach Running).
    manager.attempt_start();

    compat::CompatMemory memory;
    compat::SyscallDispatch dispatch(&memory);
    std::vector<compat::SyscallDispatch::MatrixEntry> matrix = dispatch.matrix();

    ScreenId screen = ScreenId::kMenu;
    int matrix_page = 0;
    bool running = true;
    platform::PadState pad{};
    bool prev_cross = false;
    bool prev_options = false;
    bool prev_circle = false;
    bool prev_up = false;
    bool prev_down = false;
    bool prev_l1 = false;
    bool prev_r1 = false;

    while (running) {
        platform::Framebuffer* fb = display->front_buffer();
        if (fb == nullptr)
            break;

        switch (screen) {
        case ScreenId::kMenu:
            show_menu(fb, launcher);
            break;
        case ScreenId::kProbes:
            show_screen(fb, "PROBES", ui::probe_screen_lines(report, kMaxLines));
            break;
        case ScreenId::kStatus:
            show_screen(fb, "STATUS",
                        ui::status_screen_lines(manager.phase(), manager.last_blockers(),
                                                platform::backend_name(), ps5droid::kVersion,
                                                kMaxLines));
            break;
        case ScreenId::kMatrix: {
            ui::MatrixPage page = ui::matrix_screen_lines(matrix, matrix_page, kMaxLines);
            matrix_page = page.page;
            show_screen(fb, "LINUX ABI", page.lines);
            break;
        }
        case ScreenId::kAbout:
            show_screen(fb, "ABOUT", ui::about_screen_lines(ps5droid::kVersion));
            break;
        }

        display->present();

        if (input->poll(&pad)) {
            bool edge_up = pad.up && !prev_up;
            bool edge_down = pad.down && !prev_down;
            bool edge_cross = pad.cross && !prev_cross;
            bool edge_circle = pad.circle && !prev_circle;
            bool edge_options = pad.options && !prev_options;
            bool edge_l1 = pad.l1 && !prev_l1;
            bool edge_r1 = pad.r1 && !prev_r1;

            if (screen == ScreenId::kMenu) {
                if (edge_up)
                    launcher.move_up();
                if (edge_down)
                    launcher.move_down();
                if (edge_cross) {
                    ui::LauncherAction action = launcher.activate();
                    switch (action) {
                    case ui::LauncherAction::kExit:
                        running = false;
                        break;
                    case ui::LauncherAction::kShowProbes:
                        screen = ScreenId::kProbes;
                        break;
                    case ui::LauncherAction::kShowStatus:
                        screen = ScreenId::kStatus;
                        break;
                    case ui::LauncherAction::kShowCompatMatrix:
                        screen = ScreenId::kMatrix;
                        break;
                    case ui::LauncherAction::kShowAbout:
                        screen = ScreenId::kAbout;
                        break;
                    case ui::LauncherAction::kShowAndroidEnv:
                        // The environment screen IS the honest status: the
                        // start attempt is always blocked today.
                        screen = ScreenId::kStatus;
                        break;
                    default:
                        break;
                    }
                }
            } else {
                if (edge_circle || edge_cross)
                    screen = ScreenId::kMenu;
                if (screen == ScreenId::kMatrix) {
                    if (edge_l1 && matrix_page > 0)
                        --matrix_page;
                    if (edge_r1)
                        ++matrix_page; // clamped by matrix_screen_lines
                }
            }

            if (edge_options)
                running = false;
            prev_up = pad.up;
            prev_down = pad.down;
            prev_cross = pad.cross;
            prev_circle = pad.circle;
            prev_options = pad.options;
            prev_l1 = pad.l1;
            prev_r1 = pad.r1;
        }

        platform::sleep_ns(1000000); // 1 ms; flips are vsync-locked
    }
}

} // namespace

int main() {
    ps5::sceSystemServiceHideSplashScreen();

    std::unique_ptr<platform::DisplayBackend> display(platform::create_ps5_display());
    std::unique_ptr<platform::InputBackend> input(platform::create_ps5_input());

    uint32_t w = 0, h = 0;
    if (!display->init(&w, &h)) {
        ps5::SceNotificationRequest req;
        memset(&req, 0, sizeof(req));
        strncpy(req.message, "PS5Droid: video-out init failed", sizeof(req.message) - 1);
        ps5::sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
        return 1;
    }

    if (!input->init()) {
        ps5::SceNotificationRequest req;
        memset(&req, 0, sizeof(req));
        strncpy(req.message, "PS5Droid: pad init failed", sizeof(req.message) - 1);
        ps5::sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
        display->shutdown();
        return 2;
    }

    run_launcher(display.get(), input.get());

    display->shutdown();
    return 0;
}
