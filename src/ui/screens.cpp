// PS5Droid - screen content builders (implementation).
#include "ui/screens.h"

#include <cstdio>
#include <cstring>

#include "compat/linux_abi.h"

namespace ps5droid {
namespace ui {

namespace {

std::string phase_name(android::RuntimePhase phase) {
    switch (phase) {
    case android::RuntimePhase::kNotInitialized:
        return "NOT INITIALIZED";
    case android::RuntimePhase::kProbing:
        return "PROBING";
    case android::RuntimePhase::kPlatformVerified:
        return "PLATFORM VERIFIED";
    case android::RuntimePhase::kCompatReady:
        return "COMPAT READY";
    case android::RuntimePhase::kStarting:
        return "STARTING";
    case android::RuntimePhase::kBlocked:
        return "BLOCKED";
    case android::RuntimePhase::kRunning:
        return "RUNNING (UNREACHABLE TODAY)";
    }
    return "UNKNOWN";
}

const char* support_name(linux_abi::Support s) {
    switch (s) {
    case linux_abi::kEmulated:
        return "EMU";
    case linux_abi::kPartial:
        return "PART";
    case linux_abi::kUnsupported:
        return "NO";
    }
    return "?";
}

} // namespace

std::vector<ScreenLine> probe_screen_lines(const android::CapabilityReport& report, int max_lines) {
    std::vector<ScreenLine> out;
    if (max_lines <= 0)
        return out;

    std::string header = "BACKEND: ";
    header += report.backend;
    out.emplace_back(std::move(header));
    for (const auto& r : report.results) {
        if (static_cast<int>(out.size()) >= max_lines)
            break;
        std::string line = r.name;
        line += r.passed ? " [OK] " : " [FAIL] ";
        line += r.detail;
        for (char& c : line) {
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        }
        out.emplace_back(std::move(line));
    }
    if (static_cast<int>(out.size()) < max_lines) {
        out.emplace_back(report.all_passed ? "ALL PROBES PASSED" : "SOME PROBES FAILED", true);
    }
    return out;
}

std::vector<ScreenLine> status_screen_lines(android::RuntimePhase phase,
                                            const std::vector<android::Blocker>& blockers,
                                            const char* backend, const char* version,
                                            int max_lines) {
    std::vector<ScreenLine> out;
    if (max_lines <= 0)
        return out;

    std::string line = "PHASE: ";
    line += phase_name(phase);
    out.emplace_back(std::move(line));
    line = "BACKEND: ";
    line += backend ? backend : "?";
    out.emplace_back(std::move(line));
    line = "VERSION: ";
    line += version ? version : "?";
    out.emplace_back(std::move(line));

    for (const auto& b : blockers) {
        if (static_cast<int>(out.size()) >= max_lines)
            break;
        std::string bline = "[BLOCKED] ";
        bline += b.component;
        for (char& c : bline) {
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        }
        out.emplace_back(std::move(bline));
    }
    return out;
}

MatrixPage matrix_screen_lines(const std::vector<compat::SyscallDispatch::MatrixEntry>& matrix,
                               int page, int max_lines) {
    MatrixPage out;

    int rows_per_page = max_lines - 3; // header + summary + pager footer
    if (rows_per_page < 1)
        rows_per_page = 1;
    int total_rows = static_cast<int>(matrix.size());
    out.page_count = (total_rows + rows_per_page - 1) / rows_per_page;
    if (out.page_count < 1)
        out.page_count = 1;
    if (page < 0)
        page = 0;
    if (page >= out.page_count)
        page = out.page_count - 1;
    out.page = page;

    int emulated = 0, partial = 0, unsupported = 0;
    for (const auto& e : matrix) {
        switch (e.support) {
        case linux_abi::kEmulated:
            ++emulated;
            break;
        case linux_abi::kPartial:
            ++partial;
            break;
        case linux_abi::kUnsupported:
            ++unsupported;
            break;
        }
    }

    char header[96];
    snprintf(header, sizeof(header), "LINUX ABI MATRIX: %d TRACKED", total_rows);
    out.lines.emplace_back(header);
    char summary[96];
    snprintf(summary, sizeof(summary), "EMU %d / PART %d / UNSUP %d", emulated, partial,
             unsupported);
    out.lines.emplace_back(summary);

    int begin = page * rows_per_page;
    int end = begin + rows_per_page;
    if (end > total_rows)
        end = total_rows;
    for (int i = begin; i < end; ++i) {
        const auto& e = matrix[static_cast<size_t>(i)];
        char row[96];
        snprintf(row, sizeof(row), "%-18s %-4s %3d", e.name ? e.name : "?", support_name(e.support),
                 e.number);
        out.lines.emplace_back(row);
    }

    char pager[48];
    snprintf(pager, sizeof(pager), "PAGE %d/%d  L1-R1 TURN", page + 1, out.page_count);
    out.lines.emplace_back(pager);
    return out;
}

std::vector<ScreenLine> about_screen_lines(const char* version) {
    std::vector<ScreenLine> out;
    std::string v = "PS5DROID V";
    v += version ? version : "?";
    out.emplace_back(std::move(v));
    out.emplace_back("RESEARCH PROJECT - HONESTY FIRST");
    out.emplace_back("GOAL: NATIVE ANDROID RUNTIME STUDY");
    out.emplace_back("STATUS: NO ANDROID APPS RUN YET");
    out.emplace_back("SEE DOCS AND README FOR EVIDENCE");
    return out;
}

} // namespace ui
} // namespace ps5droid
