// PS5Droid - Linux ABI dispatch tests.
#include "test_framework.h"

#include <cstring>

#include "compat/memory_compat.h"
#include "compat/syscall_dispatch.h"
#include "platform/platform.h"

using namespace ps5droid::compat;
using namespace ps5droid::linux_abi;

namespace {
// A single mapping returned by the dispatch (Linux mmap=9).
void* mmap_via(SyscallDispatch* d, size_t len) {
    SyscallArgs a{};
    a.a[1] = len;
    a.a[2] = kProtRead | kProtWrite;
    a.a[3] = kMapPrivate | kMapAnonymous;
    a.a[4] = static_cast<uint64_t>(-1);
    DispatchResult r = d->invoke(kMmap, a);
    return r.ret > 0 ? reinterpret_cast<void*>(r.ret) : nullptr;
}
} // namespace

PS5DROID_TEST(mmap_munmap_end_to_end) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    void* p = mmap_via(&d, 8192);
    CHECK_MSG(p != nullptr, "mmap(9) failed");

    uint8_t* bytes = static_cast<uint8_t*>(p);
    memset(bytes, 0x5A, 8192);
    CHECK_MSG(bytes[0] == 0x5A && bytes[8191] == 0x5A, "memory not usable");

    SyscallArgs u{};
    u.a[0] = reinterpret_cast<uint64_t>(p);
    u.a[1] = 8192;
    DispatchResult r = d.invoke(kMunmap, u);
    CHECK_MSG(r.ret == 0, "munmap(11) failed");
}

PS5DROID_TEST(mmap_validates_arguments) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    SyscallArgs a{};
    a.a[1] = 4096;
    a.a[3] = kMapPrivate; // missing MAP_ANONYMOUS
    a.a[4] = static_cast<uint64_t>(-1);
    DispatchResult r = d.invoke(kMmap, a);
    CHECK_MSG(r.ret == -1 && r.err == kEinval, "must reject file-backed");

    SyscallArgs b{};
    b.a[1] = 0; // zero length
    b.a[3] = kMapPrivate | kMapAnonymous;
    r = d.invoke(kMmap, b);
    CHECK_MSG(r.ret == -1 && r.err == kEinval, "must reject zero length");

    SyscallArgs c{};
    c.a[1] = 4096;
    c.a[3] = kMapPrivate | kMapAnonymous | kMapFixed;
    c.a[4] = static_cast<uint64_t>(-1);
    r = d.invoke(kMmap, c);
    CHECK_MSG(r.ret == -1 && r.err == kEinval, "must reject MAP_FIXED");
}

PS5DROID_TEST(brk_semantics) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    SyscallArgs a{};
    DispatchResult r = d.invoke(kBrk, a); // brk(0) initializes heap
    CHECK_MSG(r.ret > 0, "brk(0) must return the initial break");
    uintptr_t start = static_cast<uintptr_t>(r.ret);

    SyscallArgs b{};
    b.a[0] = start + 4096;
    r = d.invoke(kBrk, b);
    CHECK_MSG(static_cast<uintptr_t>(r.ret) == start + 4096, "brk must grow");

    // The grown region must be writable.
    uint8_t* p = reinterpret_cast<uint8_t*>(start + 8);
    p[0] = 0x42;
    p[4000] = 0x43;
    CHECK_MSG(p[0] == 0x42 && p[4000] == 0x43, "brk region not writable");

    // Overflowing request returns the current break (Linux behavior).
    SyscallArgs c{};
    c.a[0] = start + (1ull << 40);
    r = d.invoke(kBrk, c);
    CHECK_MSG(static_cast<uintptr_t>(r.ret) == start + 4096,
              "failed brk must return current break");
}

PS5DROID_TEST(clock_gettime_advances) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    static uint64_t buf[2];
    SyscallArgs a{};
    a.a[0] = kClockMonotonic;
    a.a[1] = reinterpret_cast<uint64_t>(buf);
    DispatchResult r = d.invoke(kClockGettime, a);
    CHECK_MSG(r.ret == 0, "clock_gettime failed");
    uint64_t first = static_cast<uint64_t>(buf[0]) * 1000000000ull + static_cast<uint64_t>(buf[1]);
    CHECK_MSG(first > 0, "monotonic clock must be positive");

    ps5droid::platform::sleep_ns(2000000); // 2 ms
    r = d.invoke(kClockGettime, a);
    uint64_t second = static_cast<uint64_t>(buf[0]) * 1000000000ull + static_cast<uint64_t>(buf[1]);
    CHECK_MSG(second > first, "monotonic clock must advance");
}

PS5DROID_TEST(getpid_gettid) {
    CompatMemory mem;
    SyscallDispatch d(&mem);
    DispatchResult pid = d.invoke(kGetpid, {});
    DispatchResult tid = d.invoke(kGettid, {});
    CHECK_MSG(pid.ret > 0, "getpid must return the context id");
    CHECK_MSG(tid.ret > 0, "gettid must return a thread id");

    CompatMemory mem2;
    SyscallDispatch d2(&mem2);
    DispatchResult pid2 = d2.invoke(kGetpid, {});
    CHECK_MSG(pid2.ret != pid.ret, "contexts must have distinct ids");
}

PS5DROID_TEST(futex_via_dispatch) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    static uint32_t word;
    word = 100;

    // EWOULDBLOCK: value mismatch
    SyscallArgs a{};
    a.a[0] = reinterpret_cast<uint64_t>(&word);
    a.a[1] = kFutexWait | kFutexPrivateFlag;
    a.a[2] = 999;
    DispatchResult r = d.invoke(kFutex, a);
    CHECK_MSG(r.ret == 1, "expected EWOULDBLOCK");

    // No-waiter wake returns 0
    SyscallArgs b{};
    b.a[0] = reinterpret_cast<uint64_t>(&word);
    b.a[1] = kFutexWake | kFutexPrivateFlag;
    b.a[2] = 1;
    r = d.invoke(kFutex, b);
    CHECK_MSG(r.ret == 0, "expected zero waiters");

    // Non-private and unsupported ops must be refused
    SyscallArgs c{};
    c.a[0] = reinterpret_cast<uint64_t>(&word);
    c.a[1] = kFutexWake; // missing PRIVATE flag
    r = d.invoke(kFutex, c);
    CHECK_MSG(r.ret == -1 && r.err == kEinval, "must refuse non-private");
}

PS5DROID_TEST(write_passthrough) {
    CompatMemory mem;
    SyscallDispatch d(&mem);
    SyscallArgs a{};
    const char* msg = "";
    a.a[0] = 1;
    a.a[1] = reinterpret_cast<uint64_t>(msg);
    a.a[2] = 0;
    DispatchResult r = d.invoke(kWrite, a);
    CHECK_MSG(r.ret == 0, "empty write should write 0 bytes");

    // fd 3 does not exist in the emulation model
    a.a[0] = 3;
    r = d.invoke(kWrite, a);
    CHECK_MSG(r.ret == -1 && r.err == kEbadf, "fd 3 must be EBADF");
}

PS5DROID_TEST(honest_enosys_for_blockers) {
    CompatMemory mem;
    SyscallDispatch d(&mem);
    const int blocked[] = {kClone, kFork, kExecve, kSocket, kUname, kPrctl, kGetuid};
    for (int nr : blocked) {
        SyscallArgs a{};
        DispatchResult r = d.invoke(nr, a);
        char msg[96];
        snprintf(msg, sizeof(msg), "syscall %d must be ENOSYS", nr);
        CHECK_MSG(r.ret == -1 && r.err == kEnosys && !r.supported, msg);
    }
}

PS5DROID_TEST(fd_table_and_epoll_dispatch) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    std::string tmp = "ps5droid_fd_test.tmp";
    SyscallArgs open_args{};
    // Linux x86_64 O_CREAT(0x40)|O_TRUNC(0x200)|O_RDWR(2) -- NOT the host
    // platform's O_* values; the guest ABI is Linux.
    open_args.a[0] = reinterpret_cast<uint64_t>(tmp.c_str());
    open_args.a[1] = kOCreat | kOTrunc | kORdwr;
    DispatchResult r = d.invoke(kOpen, open_args);
    CHECK_MSG(r.ret >= 3, "open should return valid guest fd");
    int fd = static_cast<int>(r.ret);

    const char* data = "hello fd_table";
    SyscallArgs write_args{};
    write_args.a[0] = fd;
    write_args.a[1] = reinterpret_cast<uint64_t>(data);
    write_args.a[2] = strlen(data);
    r = d.invoke(kWrite, write_args);
    CHECK_MSG(r.ret == static_cast<int64_t>(strlen(data)), "write to fd failed");

    SyscallArgs seek_args{};
    seek_args.a[0] = fd;
    seek_args.a[1] = 0;
    seek_args.a[2] = 0; // SEEK_SET
    r = d.invoke(kLseek, seek_args);
    CHECK_MSG(r.ret == 0, "lseek failed");

    char buf[64] = {0};
    SyscallArgs read_args{};
    read_args.a[0] = fd;
    read_args.a[1] = reinterpret_cast<uint64_t>(buf);
    read_args.a[2] = sizeof(buf);
    r = d.invoke(kRead, read_args);
    CHECK_MSG(r.ret == static_cast<int64_t>(strlen(data)) && strcmp(buf, data) == 0,
              "read from fd failed");

    SyscallArgs ep_create{};
    r = d.invoke(kEpollCreate1, ep_create);
    CHECK_MSG(r.ret >= 3, "epoll_create1 failed");
    int epfd = static_cast<int>(r.ret);

    EpollEvent ev{1 /* EPOLLIN */, 12345};
    SyscallArgs ep_ctl{};
    ep_ctl.a[0] = epfd;
    ep_ctl.a[1] = 1; // EPOLL_CTL_ADD
    ep_ctl.a[2] = fd;
    ep_ctl.a[3] = reinterpret_cast<uint64_t>(&ev);
    r = d.invoke(kEpollCtl, ep_ctl);
    CHECK_MSG(r.ret == 0, "epoll_ctl failed");

    EpollEvent out_evs[4];
    SyscallArgs ep_wait{};
    ep_wait.a[0] = epfd;
    ep_wait.a[1] = reinterpret_cast<uint64_t>(out_evs);
    ep_wait.a[2] = 4;
    ep_wait.a[3] = 0;
    r = d.invoke(kEpollWait, ep_wait);
    CHECK_MSG(r.ret == 1 && out_evs[0].data == 12345, "epoll_wait failed");

    SyscallArgs close_args{};
    close_args.a[0] = fd;
    d.invoke(kClose, close_args);
    close_args.a[0] = epfd;
    d.invoke(kClose, close_args);
    ps5droid::platform::remove_file(tmp);
}

PS5DROID_TEST(exit_group_sets_flag) {
    CompatMemory mem;
    SyscallDispatch d(&mem);
    CHECK_MSG(!d.stop_requested(), "must start unrequested");
    DispatchResult r = d.invoke(kExitGroup, {});
    CHECK_MSG(r.ret == 0, "exit_group returns 0");
    CHECK_MSG(d.stop_requested(), "exit_group must flag the context");
}

namespace {
// Guest iovec layout on x86_64: { void* base; size_t len; } (16 bytes).
struct GuestIovec {
    void* base;
    uint64_t len;
};
} // namespace

PS5DROID_TEST(mmap_requires_map_private) {
    // Linux requires exactly one of MAP_SHARED/MAP_PRIVATE; the experiment
    // models the private anonymous flavor only.
    CompatMemory mem;
    SyscallDispatch d(&mem);

    SyscallArgs a{};
    a.a[1] = 4096;
    a.a[2] = kProtRead | kProtWrite;
    a.a[3] = kMapAnonymous; // missing MAP_PRIVATE
    a.a[4] = static_cast<uint64_t>(-1);
    DispatchResult r = d.invoke(kMmap, a);
    CHECK_MSG(r.ret == -1 && r.err == kEinval, "MAP_PRIVATE must be required");
}

PS5DROID_TEST(open_flags_are_linux_values) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    // O_EXCL|O_CREAT on an existing file must fail with EEXIST (Linux).
    std::string tmp = "ps5droid_open_flags.tmp";
    SyscallArgs a{};
    a.a[0] = reinterpret_cast<uint64_t>(tmp.c_str());
    a.a[1] = kOCreat | kOExcl | kOWronly;
    a.a[2] = 0644;
    DispatchResult r = d.invoke(kOpen, a);
    CHECK_MSG(r.ret >= 3, "first exclusive open must succeed");
    int fd = static_cast<int>(r.ret);

    DispatchResult r2 = d.invoke(kOpen, a);
    CHECK_MSG(r2.ret == -1 && r2.err == kEexist, "second exclusive open must EEXIST");

    // Plain open of a missing file without O_CREAT must fail ENOENT.
    SyscallArgs b{};
    b.a[0] = reinterpret_cast<uint64_t>("ps5droid_missing_file_definitely_absent");
    b.a[1] = kORdonly;
    DispatchResult r3 = d.invoke(kOpen, b);
    CHECK_MSG(r3.ret == -1 && r3.err == kEnoent, "missing file must ENOENT");

    // openat with a real dirfd is refused (only AT_FDCWD supported).
    SyscallArgs c{};
    c.a[0] = static_cast<uint64_t>(static_cast<int64_t>(5)); // dirfd != AT_FDCWD
    c.a[1] = reinterpret_cast<uint64_t>(tmp.c_str());
    c.a[2] = kORdonly;
    DispatchResult r4 = d.invoke(kOpenat, c);
    CHECK_MSG(r4.ret == -1 && r4.err == kEbadf, "non-AT_FDCWD dirfd must EBADF");

    // Lowest-free fd allocation: after closing fd 3 the next open reuses it.
    SyscallArgs cl{};
    cl.a[0] = static_cast<uint64_t>(fd);
    CHECK_MSG(d.invoke(kClose, cl).ret == 0, "close failed");
    SyscallArgs re{};
    re.a[0] = reinterpret_cast<uint64_t>(tmp.c_str());
    re.a[1] = kOCreat | kOTrunc | kORdwr;
    DispatchResult r5 = d.invoke(kOpen, re);
    CHECK_MSG(r5.ret == fd, "lowest free fd must be reused like Linux");

    SyscallArgs cl2{};
    cl2.a[0] = static_cast<uint64_t>(fd);
    d.invoke(kClose, cl2);
    ps5droid::platform::remove_file(tmp);
}

PS5DROID_TEST(epoll_ctl_semantics) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    std::string tmp = "ps5droid_epoll_sem.tmp";
    SyscallArgs oa{};
    oa.a[0] = reinterpret_cast<uint64_t>(tmp.c_str());
    oa.a[1] = kOCreat | kOTrunc | kORdwr;
    int fd = static_cast<int>(d.invoke(kOpen, oa).ret);
    CHECK_MSG(fd >= 3, "open failed");

    SyscallArgs ca{};
    ca.a[0] = static_cast<uint64_t>(d.invoke(kEpollCreate1, {}).ret);
    CHECK_MSG(ca.a[0] >= 3, "epoll_create1 failed");
    int epfd = static_cast<int>(ca.a[0]);

    EpollEvent ev{1 /* EPOLLIN */, 777};
    SyscallArgs add{};
    add.a[0] = epfd;
    add.a[1] = kEpollCtlAdd;
    add.a[2] = static_cast<uint64_t>(fd);
    add.a[3] = reinterpret_cast<uint64_t>(&ev);
    CHECK_MSG(d.invoke(kEpollCtl, add).ret == 0, "ADD failed");

    // Duplicate ADD must fail with EEXIST (Linux semantics).
    DispatchResult dup = d.invoke(kEpollCtl, add);
    CHECK_MSG(dup.ret == -1 && dup.err == kEexist, "duplicate ADD must EEXIST");

    // MOD of an unregistered fd must fail with ENOENT.
    SyscallArgs mod{};
    mod.a[0] = epfd;
    mod.a[1] = kEpollCtlMod;
    mod.a[2] = static_cast<uint64_t>(fd + 1); // never registered
    mod.a[3] = reinterpret_cast<uint64_t>(&ev);
    DispatchResult m = d.invoke(kEpollCtl, mod);
    CHECK_MSG(m.ret == -1 && m.err == kEnoent, "MOD of unregistered must ENOENT");

    // DEL then DEL again: second DEL must ENOENT.
    SyscallArgs del{};
    del.a[0] = epfd;
    del.a[1] = kEpollCtlDel;
    del.a[2] = static_cast<uint64_t>(fd);
    CHECK_MSG(d.invoke(kEpollCtl, del).ret == 0, "DEL failed");
    DispatchResult del2 = d.invoke(kEpollCtl, del);
    CHECK_MSG(del2.ret == -1 && del2.err == kEnoent, "double DEL must ENOENT");

    SyscallArgs cl{};
    cl.a[0] = static_cast<uint64_t>(fd);
    d.invoke(kClose, cl);
    cl.a[0] = epfd;
    d.invoke(kClose, cl);
    ps5droid::platform::remove_file(tmp);
}

PS5DROID_TEST(epoll_wait_is_readiness_based) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    int epfd = static_cast<int>(d.invoke(kEpollCreate1, {}).ret);
    CHECK_MSG(epfd >= 3, "epoll_create1 failed");

    int evfd = static_cast<int>(d.invoke(kEventfd2, {}).ret);
    CHECK_MSG(evfd >= 3, "eventfd2 failed");

    EpollEvent ev{1 /* EPOLLIN */, 4242};
    SyscallArgs add{};
    add.a[0] = epfd;
    add.a[1] = kEpollCtlAdd;
    add.a[2] = static_cast<uint64_t>(evfd);
    add.a[3] = reinterpret_cast<uint64_t>(&ev);
    CHECK_MSG(d.invoke(kEpollCtl, add).ret == 0, "ADD failed");

    // Counter is zero: NOT readable -> epoll_wait(0) must return 0 events.
    EpollEvent out[2];
    SyscallArgs wait{};
    wait.a[0] = epfd;
    wait.a[1] = reinterpret_cast<uint64_t>(out);
    wait.a[2] = 2;
    wait.a[3] = 0;
    DispatchResult r = d.invoke(kEpollWait, wait);
    CHECK_MSG(r.ret == 0, "empty eventfd must not be reported readable");

    // Write 1 via the fd: now readable.
    uint64_t one = 1;
    SyscallArgs wr{};
    wr.a[0] = static_cast<uint64_t>(evfd);
    wr.a[1] = reinterpret_cast<uint64_t>(&one);
    wr.a[2] = sizeof(one);
    CHECK_MSG(d.invoke(kWrite, wr).ret == static_cast<int64_t>(sizeof(one)), "eventfd write");

    r = d.invoke(kEpollWait, wait);
    CHECK_MSG(r.ret == 1 && out[0].data == 4242 && (out[0].events & 1) != 0,
              "ready eventfd must be reported with requested bits");

    // Drain the counter: readiness drops back to 0 events.
    uint64_t val = 0;
    SyscallArgs rd{};
    rd.a[0] = static_cast<uint64_t>(evfd);
    rd.a[1] = reinterpret_cast<uint64_t>(&val);
    rd.a[2] = sizeof(val);
    CHECK_MSG(d.invoke(kRead, rd).ret == static_cast<int64_t>(sizeof(val)), "eventfd read");
    r = d.invoke(kEpollWait, wait);
    CHECK_MSG(r.ret == 0, "drained eventfd must not be readable");

    SyscallArgs cl{};
    cl.a[0] = static_cast<uint64_t>(evfd);
    d.invoke(kClose, cl);
    cl.a[0] = static_cast<uint64_t>(epfd);
    d.invoke(kClose, cl);
}

PS5DROID_TEST(eventfd_semaphore_and_overflow) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    // EFD_SEMAPHORE (flag 0x1): reads return 1 and decrement.
    SyscallArgs ev{};
    ev.a[0] = 5;
    ev.a[1] = kEfdSemaphore;
    int fd = static_cast<int>(d.invoke(kEventfd2, ev).ret);
    CHECK_MSG(fd >= 3, "semaphore eventfd creation failed");

    uint64_t val = 0;
    SyscallArgs rd{};
    rd.a[0] = static_cast<uint64_t>(fd);
    rd.a[1] = reinterpret_cast<uint64_t>(&val);
    rd.a[2] = sizeof(val);
    CHECK_MSG(d.invoke(kRead, rd).ret == 8 && val == 1, "semaphore read must return 1");
    d.invoke(kRead, rd);
    CHECK_MSG(val == 1, "second semaphore read must also return 1");

    // Overflow: writing near the ceiling must fail with EINVAL.
    SyscallArgs big{};
    big.a[0] = static_cast<uint64_t>(fd);
    big.a[1] = reinterpret_cast<uint64_t>(&kEventfdMax);
    big.a[2] = sizeof(uint64_t);
    DispatchResult of = d.invoke(kWrite, big);
    CHECK_MSG(of.ret == -1 && of.err == kEinval, "overflow write must EINVAL");

    SyscallArgs cl{};
    cl.a[0] = static_cast<uint64_t>(fd);
    d.invoke(kClose, cl);
}

PS5DROID_TEST(pread64_does_not_move_offset) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    std::string tmp = "ps5droid_pread.tmp";
    SyscallArgs oa{};
    oa.a[0] = reinterpret_cast<uint64_t>(tmp.c_str());
    oa.a[1] = kOCreat | kOTrunc | kORdwr;
    int fd = static_cast<int>(d.invoke(kOpen, oa).ret);
    CHECK_MSG(fd >= 3, "open failed");

    const char* payload = "ABCDEFGHIJ";
    SyscallArgs wr{};
    wr.a[0] = static_cast<uint64_t>(fd);
    wr.a[1] = reinterpret_cast<uint64_t>(payload);
    wr.a[2] = strlen(payload);
    CHECK_MSG(d.invoke(kWrite, wr).ret == 10, "seed write failed");

    // Positioned read at offset 2; the sequential offset stays at 10.
    char buf[4] = {0};
    SyscallArgs pr{};
    pr.a[0] = static_cast<uint64_t>(fd);
    pr.a[1] = reinterpret_cast<uint64_t>(buf);
    pr.a[2] = 3;
    pr.a[3] = 2;
    CHECK_MSG(d.invoke(kPread64, pr).ret == 3, "pread64 failed");
    CHECK_MSG(strncmp(buf, "CDE", 3) == 0, "pread64 read wrong bytes");

    // Sequential read continues at 10 (EOF) - offset was not disturbed.
    char buf2[4] = {0};
    SyscallArgs sr{};
    sr.a[0] = static_cast<uint64_t>(fd);
    sr.a[1] = reinterpret_cast<uint64_t>(buf2);
    sr.a[2] = 4;
    CHECK_MSG(d.invoke(kRead, sr).ret == 0, "offset moved by pread64 (bug)");

    SyscallArgs cl{};
    cl.a[0] = static_cast<uint64_t>(fd);
    d.invoke(kClose, cl);
    ps5droid::platform::remove_file(tmp);
}

PS5DROID_TEST(writev_emulates_gather_output) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    std::string tmp = "ps5droid_writev.tmp";
    SyscallArgs oa{};
    oa.a[0] = reinterpret_cast<uint64_t>(tmp.c_str());
    oa.a[1] = kOCreat | kOTrunc | kORdwr;
    int fd = static_cast<int>(d.invoke(kOpen, oa).ret);
    CHECK_MSG(fd >= 3, "open failed");

    const char* part1 = "AB";
    const char* part2 = "CDE";
    GuestIovec iov[2];
    iov[0] = {const_cast<char*>(part1), 2};
    iov[1] = {const_cast<char*>(part2), 3};

    SyscallArgs wv{};
    wv.a[0] = static_cast<uint64_t>(fd);
    wv.a[1] = reinterpret_cast<uint64_t>(iov);
    wv.a[2] = 2;
    DispatchResult r = d.invoke(kWritev, wv);
    CHECK_MSG(r.ret == 5, "writev must return total bytes");

    SyscallArgs seek{};
    seek.a[0] = static_cast<uint64_t>(fd);
    seek.a[2] = 0; // SEEK_SET
    r = d.invoke(kLseek, seek);
    CHECK_MSG(r.ret == 0, "lseek failed");

    char back[8] = {0};
    SyscallArgs rd{};
    rd.a[0] = static_cast<uint64_t>(fd);
    rd.a[1] = reinterpret_cast<uint64_t>(back);
    rd.a[2] = 8;
    CHECK_MSG(d.invoke(kRead, rd).ret == 5 && strcmp(back, "ABCDE") == 0,
              "writev content mismatch");

    // iovcnt over IOV_MAX must be refused.
    SyscallArgs huge{};
    huge.a[0] = static_cast<uint64_t>(fd);
    huge.a[1] = reinterpret_cast<uint64_t>(iov);
    huge.a[2] = static_cast<uint64_t>(kIovMax + 1);
    r = d.invoke(kWritev, huge);
    CHECK_MSG(r.ret == -1 && r.err == kEinval, "iovcnt > IOV_MAX must EINVAL");

    SyscallArgs cl{};
    cl.a[0] = static_cast<uint64_t>(fd);
    d.invoke(kClose, cl);
    ps5droid::platform::remove_file(tmp);
}
