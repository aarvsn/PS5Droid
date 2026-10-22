// PS5Droid - v1.0.2 filesystem/time/entropy syscall tests.
//
// Every syscall exercised here is classified EMULATED (or PARTIAL for
// getrandom) in the matrix: the tests prove real behavior, not stubs.
#include "test_framework.h"

#include <cstring>
#include <unistd.h>

#include "compat/memory_compat.h"
#include "compat/syscall_dispatch.h"
#include "platform/platform.h"

using namespace ps5droid::compat;
using namespace ps5droid::linux_abi;

namespace {
// Guest struct layouts (x86_64): time_t + long fields.
struct GuestTimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
struct GuestTimeval {
    int64_t tv_sec;
    int64_t tv_usec;
};
} // namespace

PS5DROID_TEST(stat_and_newfstatat_report_file_facts) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    std::string tmp = "ps5droid_stat_probe.tmp";
    const char* payload = "0123456789";
    CHECK_MSG(ps5droid::platform::write_file(tmp, payload, strlen(payload)),
              "fixture write failed");

    StatBuf st;
    SyscallArgs a{};
    a.a[0] = reinterpret_cast<uint64_t>(tmp.c_str());
    a.a[1] = reinterpret_cast<uint64_t>(&st);
    DispatchResult r = d.invoke(kStat, a);
    CHECK_MSG(r.ret == 0, "stat failed");
    CHECK_MSG(st.st_size == static_cast<int64_t>(strlen(payload)), "stat size wrong");
    CHECK_MSG((st.st_mode & 0170000) == 0100000, "stat mode must be a regular file");

    // newfstatat with dirfd == AT_FDCWD behaves like stat.
    StatBuf st2;
    SyscallArgs b{};
    b.a[0] = static_cast<uint64_t>(static_cast<int64_t>(kAtFdcwd));
    b.a[1] = reinterpret_cast<uint64_t>(tmp.c_str());
    b.a[2] = reinterpret_cast<uint64_t>(&st2);
    b.a[3] = 0;
    r = d.invoke(kNewfstatat, b);
    CHECK_MSG(r.ret == 0 && st2.st_size == st.st_size, "newfstatat must match stat");

    // Missing path -> ENOENT; non-AT_FDCWD dirfd -> EBADF.
    SyscallArgs c{};
    c.a[0] = reinterpret_cast<uint64_t>("ps5droid_no_such_file_here");
    c.a[1] = reinterpret_cast<uint64_t>(&st);
    r = d.invoke(kStat, c);
    CHECK_MSG(r.ret == -1 && r.err == kEnoent, "stat of missing path must ENOENT");

    SyscallArgs e{};
    e.a[0] = static_cast<uint64_t>(static_cast<int64_t>(7)); // dirfd != AT_FDCWD
    e.a[1] = reinterpret_cast<uint64_t>(tmp.c_str());
    e.a[2] = reinterpret_cast<uint64_t>(&st);
    r = d.invoke(kNewfstatat, e);
    CHECK_MSG(r.ret == -1 && r.err == kEbadf, "non-AT_FDCWD dirfd must EBADF");

    ps5droid::platform::remove_file(tmp);
}

PS5DROID_TEST(access_reports_permissions) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    std::string tmp = "ps5droid_access_probe.tmp";
    CHECK_MSG(ps5droid::platform::write_file(tmp, "x", 1), "fixture write failed");

    SyscallArgs a{};
    a.a[0] = reinterpret_cast<uint64_t>(tmp.c_str());
    a.a[1] = kROk;
    CHECK_MSG(d.invoke(kAccess, a).ret == 0, "F exists: R_OK must pass");

    a.a[1] = kROk | kWOk;
    CHECK_MSG(d.invoke(kAccess, a).ret == 0, "R_OK|W_OK must pass for writable file");

    SyscallArgs b{};
    b.a[0] = reinterpret_cast<uint64_t>("ps5droid_access_missing.tmp");
    b.a[1] = kFOk;
    DispatchResult r = d.invoke(kAccess, b);
    CHECK_MSG(r.ret == -1 && r.err == kEnoent, "missing path must ENOENT");

    ps5droid::platform::remove_file(tmp);
}

PS5DROID_TEST(mkdir_and_unlink_roundtrip) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    SyscallArgs mk{};
    mk.a[0] = reinterpret_cast<uint64_t>("ps5droid_mkdir_probe.dir");
    mk.a[1] = 0755;
    CHECK_MSG(d.invoke(kMkdir, mk).ret == 0, "mkdir failed");

    // Duplicate mkdir must fail (EEXIST on Linux).
    DispatchResult dup = d.invoke(kMkdir, mk);
    CHECK_MSG(dup.ret == -1 && dup.err == kEexist, "duplicate mkdir must EEXIST");

    std::string inner = std::string("ps5droid_mkdir_probe.dir") + "/f.tmp";
    CHECK_MSG(ps5droid::platform::write_file(inner, "z", 1), "write into dir failed");

    SyscallArgs rm{};
    rm.a[0] = reinterpret_cast<uint64_t>(inner.c_str());
    CHECK_MSG(d.invoke(kUnlink, rm).ret == 0, "unlink failed");
    DispatchResult again = d.invoke(kUnlink, rm);
    CHECK_MSG(again.ret == -1 && again.err == kEnoent, "double unlink must ENOENT");

    CHECK_MSG(rmdir("ps5droid_mkdir_probe.dir") == 0, "host dir cleanup failed");
}

PS5DROID_TEST(getcwd_returns_process_cwd) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    static char buf[512];
    SyscallArgs a{};
    a.a[0] = reinterpret_cast<uint64_t>(buf);
    a.a[1] = sizeof(buf);
    DispatchResult r = d.invoke(kGetcwd, a);
    CHECK_MSG(r.ret > 0, "getcwd failed");
    CHECK_MSG(buf[0] == '/', "cwd must be absolute");

    // size == 0 must be EINVAL (Linux semantics).
    SyscallArgs b{};
    b.a[0] = reinterpret_cast<uint64_t>(buf);
    b.a[1] = 0;
    r = d.invoke(kGetcwd, b);
    CHECK_MSG(r.ret == -1 && r.err == kEinval, "zero size must EINVAL");
}

PS5DROID_TEST(gettimeofday_and_clock_getres) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    static GuestTimeval tv;
    SyscallArgs a{};
    a.a[0] = reinterpret_cast<uint64_t>(&tv);
    DispatchResult r = d.invoke(kGettimeofday, a);
    CHECK_MSG(r.ret == 0, "gettimeofday failed");
    CHECK_MSG(tv.tv_sec > 1700000000, "realtime seconds must be plausible");
    CHECK_MSG(tv.tv_usec >= 0 && tv.tv_usec < 1000000, "usec range");

    static GuestTimespec res;
    SyscallArgs b{};
    b.a[0] = kClockMonotonic;
    b.a[1] = reinterpret_cast<uint64_t>(&res);
    r = d.invoke(kClockGetres, b);
    CHECK_MSG(r.ret == 0 && res.tv_sec == 0 && res.tv_nsec == 1, "monotonic resolution");

    SyscallArgs c{};
    c.a[0] = kClockRealtime;
    c.a[1] = reinterpret_cast<uint64_t>(&res);
    CHECK_MSG(d.invoke(kClockGetres, c).ret == 0, "realtime resolution");

    SyscallArgs bad{};
    bad.a[0] = 99; // unsupported clock id
    bad.a[1] = reinterpret_cast<uint64_t>(&res);
    r = d.invoke(kClockGetres, bad);
    CHECK_MSG(r.ret == -1 && r.err == kEinval, "unsupported clock must EINVAL");
}

PS5DROID_TEST(getrandom_returns_entropy) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    static uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    SyscallArgs a{};
    a.a[0] = reinterpret_cast<uint64_t>(buf);
    a.a[1] = sizeof(buf);
    DispatchResult r = d.invoke(kGetrandom, a);
    CHECK_MSG(r.ret == static_cast<int64_t>(sizeof(buf)), "getrandom short fill");

    // 32 random bytes are overwhelmingly non-zero; reject an all-zero fill.
    int ones = 0;
    for (uint8_t b : buf)
        ones += (b != 0);
    CHECK_MSG(ones > 0, "entropy must not be all zeros");

    // Zero-length request succeeds trivially; bad flags are EINVAL.
    SyscallArgs z{};
    z.a[0] = reinterpret_cast<uint64_t>(buf);
    z.a[1] = 0;
    CHECK_MSG(d.invoke(kGetrandom, z).ret == 0, "zero-length must return 0");

    SyscallArgs bad{};
    bad.a[0] = reinterpret_cast<uint64_t>(buf);
    bad.a[1] = 4;
    bad.a[2] = 0xDEAD;
    r = d.invoke(kGetrandom, bad);
    CHECK_MSG(r.ret == -1 && r.err == kEinval, "unsupported flags must EINVAL");
}
