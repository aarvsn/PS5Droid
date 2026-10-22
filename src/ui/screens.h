// PS5Droid - screen content builders (portable, testable).
//
// Presentation logic shared by the host CLI and the PS5 payload: turn
// runtime-manager and Linux-ABI data into plain text lines the bitmap
// renderer can draw. Nothing here touches a framebuffer, so every screen
// is unit-testable on the host; the PS5-only code stays a thin loop.
//
// Text convention: the 8x8 font covers A-Z, 0-9 and a small punctuation
// set (see ui/bitmap_font.h). Builders uppercase everything and stick to
// the supported punctuation so on-console output is predictable.
#pragma once

#include <string>
#include <vector>

#include "android/runtime_manager.h"
#include "compat/syscall_dispatch.h"

namespace ps5droid {
namespace ui {

struct ScreenLine {
    std::string text;
    bool highlight = false; // renderer draws a selection marker next to it

    ScreenLine() = default;
    ScreenLine(std::string t, bool h = false) : text(std::move(t)), highlight(h) {}
};

// Probe results screen (one line per capability probe).
std::vector<ScreenLine> probe_screen_lines(const android::CapabilityReport& report, int max_lines);

// Status screen: runtime phase, backend, version and the structured
// blockers from the last (honest) Android start attempt.
std::vector<ScreenLine> status_screen_lines(android::RuntimePhase phase,
                                            const std::vector<android::Blocker>& blockers,
                                            const char* backend, const char* version,
                                            int max_lines);

// Paged Linux ABI compatibility matrix screen.
struct MatrixPage {
    std::vector<ScreenLine> lines;
    int page = 0;
    int page_count = 1;
};

MatrixPage matrix_screen_lines(const std::vector<compat::SyscallDispatch::MatrixEntry>& matrix,
                               int page, int max_lines);

// About screen.
std::vector<ScreenLine> about_screen_lines(const char* version);

} // namespace ui
} // namespace ps5droid
