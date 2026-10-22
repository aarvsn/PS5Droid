// PS5Droid - host runtime entry point.
//
// This is a HOST-ONLY research binary (ps5droid-host). It is not a PS5
// executable and must never be labeled as one. It runs the platform
// capability probes, demonstrates the Linux ABI dispatch end to end and
// prints the honest compatibility matrix.
//
// The PS5 payload entry point lives in app/ps5_main.cpp and is only built
// with the ps5-payload-sdk toolchain.
#include <cstdio>
#include <cstring>

#include "android/runtime_manager.h"
#include "compat/linux_abi.h"
#include "compat/syscall_dispatch.h"
#include "platform/platform.h"
#include "version.h"

using namespace ps5droid;

namespace {

const char* support_name(linux_abi::Support s) {
    switch (s) {
    case linux_abi::kEmulated:
        return "EMULATED";
    case linux_abi::kPartial:
        return "PARTIAL";
    case linux_abi::kUnsupported:
        return "UNSUPPORTED";
    }
    return "?";
}

void print_banner() {
    printf("PS5Droid host runtime v%s - research build\n", ps5droid::kVersion);
    printf("  backend      : %s\n", platform::backend_name());
    printf("  page size    : %zu bytes\n", platform::page_size());
    printf("  NOT a PS5 executable; console builds require ps5-payload-sdk\n");
    printf("\n");
}

void run_probes() {
    android::RuntimeManager manager;
    printf("== Platform capability probes (%s backend) ==\n", platform::backend_name());
    android::CapabilityReport report = manager.probe();
    for (const auto& r : report.results) {
        printf("  [%s] %-16s %s\n", r.passed ? "OK" : "FAIL", r.name.c_str(), r.detail.c_str());
    }
    printf("  all probes passed: %s\n", report.all_passed ? "YES" : "NO");
    printf("\n");
}

void demo_linux_abi() {
    compat::CompatMemory memory;
    compat::SyscallDispatch dispatch(&memory);

    printf("== Linux ABI demonstration (in-process translation) ==\n");

    // mmap(9): anonymous private page.
    compat::SyscallArgs a{};
    a.a[1] = 16384; // length
    a.a[2] = 0x3;   // PROT_READ|PROT_WRITE
    a.a[3] = 0x22;  // MAP_PRIVATE|MAP_ANONYMOUS
    a.a[4] = static_cast<uint64_t>(-1);
    auto r = dispatch.invoke(linux_abi::kMmap, a);
    printf("  mmap(9)    -> ret=%#llx %s (%s)\n", static_cast<unsigned long long>(r.ret),
           r.ret > 0 ? "ok" : "FAILED", r.note);
    if (r.ret > 0) {
        memset(reinterpret_cast<void*>(r.ret), 0x5A, 16384);
        compat::SyscallArgs m{};
        m.a[0] = static_cast<uint64_t>(r.ret);
        m.a[1] = 16384;
        auto r2 = dispatch.invoke(linux_abi::kMunmap, m);
        printf("  munmap(11) -> ret=%lld %s\n", static_cast<long long>(r2.ret), r2.note);
    }

    // brk(12)
    compat::SyscallArgs b{};
    auto br = dispatch.invoke(linux_abi::kBrk, b);
    compat::SyscallArgs b2{};
    b2.a[0] = static_cast<uint64_t>(br.ret) + 4096;
    auto br2 = dispatch.invoke(linux_abi::kBrk, b2);
    printf("  brk(12)    -> %#llx -> %#llx %s\n", static_cast<unsigned long long>(br.ret),
           static_cast<unsigned long long>(br2.ret), br2.note);

    // write(1)
    const char* msg = "  write(1)   -> hello from the Linux ABI layer\n";
    compat::SyscallArgs w{};
    w.a[0] = 1;
    w.a[1] = reinterpret_cast<uint64_t>(msg);
    w.a[2] = strlen(msg);
    auto wr = dispatch.invoke(linux_abi::kWrite, w);
    printf("             -> wrote %lld bytes\n", static_cast<long long>(wr.ret));

    // getpid/gettid
    auto gp = dispatch.invoke(linux_abi::kGetpid, {});
    auto gt = dispatch.invoke(linux_abi::kGettid, {});
    printf("  getpid(39) -> %llu (emulation context)\n", static_cast<unsigned long long>(gp.ret));
    printf("  gettid(186)-> %u (platform thread id)\n", static_cast<uint32_t>(gt.ret));

    // honest failure
    compat::SyscallArgs x{};
    auto ex = dispatch.invoke(linux_abi::kExecve, x);
    printf("  execve(59) -> ret=%lld errno=%d (honest refusal: %s)\n",
           static_cast<long long>(ex.ret), ex.err, ex.note);
    printf("\n");
}

void print_matrix_summary() {
    compat::CompatMemory memory;
    compat::SyscallDispatch dispatch(&memory);
    auto matrix = dispatch.matrix();
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
    printf("== Linux ABI compatibility matrix (%d syscalls tracked) ==\n",
           static_cast<int>(matrix.size()));
    printf("  EMULATED=%d PARTIAL=%d UNSUPPORTED=%d\n", emulated, partial, unsupported);
    printf("  full matrix: docs/platform-constraints.md section 6, or use "
           "--matrix\n");
    printf("\n");
}

void print_full_matrix() {
    compat::CompatMemory memory;
    compat::SyscallDispatch dispatch(&memory);
    printf("%-6s %-18s %-12s %s\n", "NR", "NAME", "SUPPORT", "NOTE");
    for (const auto& e : dispatch.matrix()) {
        printf("%-6d %-18s %-12s %s\n", e.number, e.name ? e.name : "?", support_name(e.support),
               e.note ? e.note : "");
    }
    printf("\nLinux->FreeBSD/Orbis numbers verified from ps5-payload-sdk:\n");
    for (int i = 0; i < linux_abi::kFreebsdVerifiedSize; ++i) {
        const auto& m = linux_abi::kFreebsdVerified[i];
        printf("  %s(%d) -> %d  [%s]\n", linux_abi::syscall_name(m.linux_nr), m.linux_nr,
               m.freebsd_nr, m.note);
    }
}

void attempt_android_start() {
    android::RuntimeManager manager;
    manager.probe();
    android::StartAttempt attempt = manager.attempt_start();
    printf("== Android environment start attempt ==\n");
    printf("  phase after attempt: BLOCKED (expected; see blockers)\n");
    for (const auto& b : attempt.blockers) {
        printf("  [BLOCKED] %s\n            %s\n", b.component.c_str(), b.reason.c_str());
    }
    printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("ps5droid-host %s (backend: %s)\n", ps5droid::kVersion, platform::backend_name());
        return 0;
    }

    print_banner();
    run_probes();

    if (argc > 1 && strcmp(argv[1], "--matrix") == 0) {
        print_full_matrix();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--start-attempt") == 0) {
        attempt_android_start();
        return 0;
    }

    demo_linux_abi();
    print_matrix_summary();
    printf("usage: ps5droid-host [--matrix | --start-attempt | --version]\n");
    return 0;
}
