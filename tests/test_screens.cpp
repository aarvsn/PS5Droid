// PS5Droid - screen content builder tests (v1.0.2).
//
// The PS5 payload renders exactly what these builders produce, so the
// content logic is pinned here on the host where it can actually run.
#include "test_framework.h"

#include <cstring>
#include <string>
#include <vector>

#include "android/runtime_manager.h"
#include "compat/memory_compat.h"
#include "compat/syscall_dispatch.h"
#include "ui/screens.h"
#include "version.h"

using namespace ps5droid;

PS5DROID_TEST(probe_screen_lists_every_probe) {
    android::RuntimeManager manager;
    android::CapabilityReport report = manager.probe();

    auto lines = ui::probe_screen_lines(report, 64);
    CHECK_MSG(lines.size() == report.results.size() + 2, "header + one line per probe + verdict");
    CHECK_MSG(lines.back().text == "ALL PROBES PASSED", "verdict line must agree");
    CHECK_MSG(lines.back().highlight, "verdict line is highlighted");
}

PS5DROID_TEST(probe_screen_respects_max_lines) {
    android::RuntimeManager manager;
    android::CapabilityReport report = manager.probe();
    auto lines = ui::probe_screen_lines(report, 5);
    CHECK_MSG(lines.size() <= 5, "renderer line budget must be honored");
}

PS5DROID_TEST(status_screen_shows_phase_and_blockers) {
    android::RuntimeManager manager;
    manager.probe();
    android::StartAttempt attempt = manager.attempt_start();

    auto lines = ui::status_screen_lines(manager.phase(), manager.last_blockers(), "posix",
                                         ps5droid::kVersion, 64);
    CHECK_MSG(lines.size() >= 3 + attempt.blockers.size(), "phase/backend/version/blockers");
    CHECK_MSG(lines[0].text.find("BLOCKED") != std::string::npos,
              "phase after start attempt must read BLOCKED");
    CHECK_MSG(lines[2].text == std::string("VERSION: ") + ps5droid::kVersion,
              "version line must carry the release version");

    bool has_process_blocker = false;
    for (const auto& l : lines) {
        has_process_blocker |= l.text.find("[BLOCKED] PROCESS_MODEL") != std::string::npos;
    }
    CHECK_MSG(has_process_blocker, "structured blockers must be listed");
}

PS5DROID_TEST(matrix_screen_pages_and_counts) {
    compat::CompatMemory memory;
    compat::SyscallDispatch dispatch(&memory);
    auto matrix = dispatch.matrix();

    // With a 6-line budget (4 data rows per page) the full matrix must span
    // several pages and each page stays within budget.
    ui::MatrixPage p0 = ui::matrix_screen_lines(matrix, 0, 6);
    CHECK_MSG(p0.page_count > 1, "matrix must be paged for a small budget");
    CHECK_MSG(p0.page == 0, "requested page echoed");
    CHECK_MSG(p0.lines.size() <= 6, "page respects the line budget");
    CHECK_MSG(p0.lines[1].text.find("EMU 31") != std::string::npos,
              "summary must cite the release counts");

    ui::MatrixPage last = ui::matrix_screen_lines(matrix, 9999, 6);
    CHECK_MSG(last.page == p0.page_count - 1, "page request is clamped to the last page");

    ui::MatrixPage full = ui::matrix_screen_lines(matrix, 0, 200);
    CHECK_MSG(full.page_count == 1, "large budget fits the matrix on one page");
    CHECK_MSG(static_cast<int>(full.lines.size()) == static_cast<int>(matrix.size()) + 3,
              "header + summary + one row per syscall + pager");
}

PS5DROID_TEST(about_screen_carries_version) {
    auto lines = ui::about_screen_lines(ps5droid::kVersion);
    CHECK_MSG(lines.size() >= 4, "about content");
    CHECK_MSG(lines[0].text == std::string("PS5DROID V") + ps5droid::kVersion,
              "about must lead with the release version");
}
