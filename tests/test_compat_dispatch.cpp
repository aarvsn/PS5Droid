// PS5Droid - Linux ABI dispatch tests.
#include "test_framework.h"

#include <cstring>

#include "compat/memory_compat.h"
#include "compat/syscall_dispatch.h"
#include "platform/platform.h"

using namespace ps5droid::compat;
using namespace ps5droid::linux_abi;

namespace {
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
    DispatchResult r = d.invoke(kBrk, a);
    CHECK_MSG(r.ret > 0, "brk(0) must return the initial break");
    uintptr_t start = static_cast<uintptr_t>(r.ret);

    SyscallArgs b{};
    b.a[0] = start + 4096;
    r = d.invoke(kBrk, b);
    CHECK_MSG(static_cast<uintptr_t>(r.ret) == start + 4096, "brk must grow");

    uint8_t* p = reinterpret_cast<uint8_t*>(start + 8);
    p[0] = 0x42;
    p[4000] = 0x43;
    CHECK_MSG(p[0] == 0x42 && p[4000] == 0x43, "brk region not writable");

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

    ps5droid::platform::sleep_ns(2000000);
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

    SyscallArgs a{};
    a.a[0] = reinterpret_cast<uint64_t>(&word);
    a.a[1] = kFutexWait | kFutexPrivateFlag;
    a.a[2] = 999;
    DispatchResult r = d.invoke(kFutex, a);
    CHECK_MSG(r.ret == 1, "expected EWOULDBLOCK");

    SyscallArgs b{};
    b.a[0] = reinterpret_cast<uint64_t>(&word);
    b.a[1] = kFutexWake | kFutexPrivateFlag;
    b.a[2] = 1;
    r = d.invoke(kFutex, b);
    CHECK_MSG(r.ret == 0, "expected zero waiters");

    SyscallArgs c{};
    c.a[0] = reinterpret_cast<uint64_t>(&word);
    c.a[1] = kFutexWake;
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

    a.a[0] = 3;
    r = d.invoke(kWrite, a);
    CHECK_MSG(r.ret == -1 && r.err == kEbadf, "fd 3 must be EBADF");
}

PS5DROID_TEST(honest_enosys_for_blockers) {
    CompatMemory mem;
    SyscallDispatch d(&mem);
    const int blocked[] = {kClone, kFork, kExecve, kSocket, kGetppid};
    for (int nr : blocked) {
        SyscallArgs a{};
        DispatchResult r = d.invoke(nr, a);
        char msg[96];
        snprintf(msg, sizeof(msg), "syscall %d must be ENOSYS", nr);
        CHECK_MSG(r.ret == -1 && r.err == kEnosys && !r.supported, msg);
    }
}

PS5DROID_TEST(fcntl_and_identity_and_unlinkat_dispatch) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    // Identity syscalls (getuid, getgid, geteuid, getegid)
    CHECK_MSG(d.invoke(kGetuid, {}).ret == 1000, "getuid returns 1000");
    CHECK_MSG(d.invoke(kGetgid, {}).ret == 1000, "getgid returns 1000");
    CHECK_MSG(d.invoke(kGeteuid, {}).ret == 1000, "geteuid returns 1000");
    CHECK_MSG(d.invoke(kGetegid, {}).ret == 1000, "getegid returns 1000");

    // fcntl
    std::string tmp = "ps5droid_fcntl_disp.tmp";
    SyscallArgs oa{};
    oa.a[0] = reinterpret_cast<uint64_t>(tmp.c_str());
    oa.a[1] = kOCreat | kOTrunc | kORdwr;
    int fd = static_cast<int>(d.invoke(kOpen, oa).ret);
    CHECK_MSG(fd >= 3, "open failed");

    SyscallArgs f_set{};
    f_set.a[0] = fd;
    f_set.a[1] = kFSetfd;
    f_set.a[2] = kFdCloexec;
    CHECK_MSG(d.invoke(kFcntl, f_set).ret == 0, "fcntl F_SETFD failed");

    SyscallArgs f_get{};
    f_get.a[0] = fd;
    f_get.a[1] = kFGetfd;
    CHECK_MSG(d.invoke(kFcntl, f_get).ret == kFdCloexec, "fcntl F_GETFD mismatch");

    SyscallArgs cl{};
    cl.a[0] = fd;
    d.invoke(kClose, cl);

    // unlinkat file
    SyscallArgs un{};
    un.a[0] = static_cast<uint64_t>(static_cast<int64_t>(kAtFdcwd));
    un.a[1] = reinterpret_cast<uint64_t>(tmp.c_str());
    un.a[2] = 0;
    CHECK_MSG(d.invoke(kUnlinkat, un).ret == 0, "unlinkat file failed");

    // unlinkat directory with AT_REMOVEDIR
    std::string tmpdir = "ps5droid_unlinkat_dir";
    ps5droid::platform::make_dir(tmpdir, 0755);

    SyscallArgs undir{};
    undir.a[0] = static_cast<uint64_t>(static_cast<int64_t>(kAtFdcwd));
    undir.a[1] = reinterpret_cast<uint64_t>(tmpdir.c_str());
    undir.a[2] = kAtRemovedir;
    CHECK_MSG(d.invoke(kUnlinkat, undir).ret == 0, "unlinkat directory failed");
}

PS5DROID_TEST(uname_prctl_readlink_pipe2_dup3) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    // uname
    char uts[390];
    SyscallArgs u_args{};
    u_args.a[0] = reinterpret_cast<uint64_t>(uts);
    DispatchResult ur = d.invoke(kUname, u_args);
    CHECK_MSG(ur.ret == 0, "uname must succeed");
    CHECK_MSG(strcmp(uts, "Linux") == 0, "sysname must be Linux");
    CHECK_MSG(strcmp(uts + 65 * 4, "x86_64") == 0, "machine must be x86_64");

    // prctl
    SyscallArgs pr_set{};
    pr_set.a[0] = 15;
    pr_set.a[1] = reinterpret_cast<uint64_t>("test-thread");
    CHECK_MSG(d.invoke(kPrctl, pr_set).ret == 0, "prctl PR_SET_NAME must succeed");

    char name_buf[16] = {0};
    SyscallArgs pr_get{};
    pr_get.a[0] = 16;
    pr_get.a[1] = reinterpret_cast<uint64_t>(name_buf);
    CHECK_MSG(d.invoke(kPrctl, pr_get).ret == 0, "prctl PR_GET_NAME must succeed");
    CHECK_MSG(strlen(name_buf) > 0, "thread name must not be empty");

    // readlink /proc/self/exe
    char link_buf[32] = {0};
    SyscallArgs rl_args{};
    rl_args.a[0] = reinterpret_cast<uint64_t>("/proc/self/exe");
    rl_args.a[1] = reinterpret_cast<uint64_t>(link_buf);
    rl_args.a[2] = sizeof(link_buf);
    DispatchResult rlr = d.invoke(kReadlink, rl_args);
    CHECK_MSG(rlr.ret == 8 && strcmp(link_buf, "ps5droid") == 0,
              "/proc/self/exe readlink mismatch");

    // pipe2
    int pipefds[2] = {-1, -1};
    SyscallArgs p_args{};
    p_args.a[0] = reinterpret_cast<uint64_t>(pipefds);
    p_args.a[1] = 0;
    DispatchResult pr = d.invoke(kPipe2, p_args);
    CHECK_MSG(pr.ret == 0 && pipefds[0] >= 3 && pipefds[1] >= 3, "pipe2 must allocate 2 fds");

    // pipe read/write
    const char* pmsg = "pipe message";
    SyscallArgs p_wr{};
    p_wr.a[0] = pipefds[1];
    p_wr.a[1] = reinterpret_cast<uint64_t>(pmsg);
    p_wr.a[2] = strlen(pmsg);
    CHECK_MSG(d.invoke(kWrite, p_wr).ret == static_cast<int64_t>(strlen(pmsg)),
              "pipe write failed");

    char pbuf[32] = {0};
    SyscallArgs p_rd{};
    p_rd.a[0] = pipefds[0];
    p_rd.a[1] = reinterpret_cast<uint64_t>(pbuf);
    p_rd.a[2] = sizeof(pbuf);
    CHECK_MSG(d.invoke(kRead, p_rd).ret == static_cast<int64_t>(strlen(pmsg)) &&
                  strcmp(pbuf, pmsg) == 0,
              "pipe read failed");

    // dup3
    SyscallArgs d3_args{};
    d3_args.a[0] = pipefds[0];
    d3_args.a[1] = 100;
    d3_args.a[2] = 0;
    DispatchResult d3r = d.invoke(kDup3, d3_args);
    CHECK_MSG(d3r.ret == 100, "dup3 must duplicate to index 100");

    SyscallArgs cl{};
    cl.a[0] = pipefds[0];
    d.invoke(kClose, cl);
    cl.a[0] = pipefds[1];
    d.invoke(kClose, cl);
    cl.a[0] = 100;
    d.invoke(kClose, cl);
}

PS5DROID_TEST(fd_table_and_epoll_dispatch) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    std::string tmp = "ps5droid_fd_test.tmp";
    SyscallArgs open_args{};
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
    seek_args.a[2] = 0;
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

    EpollEvent ev{1, 12345};
    SyscallArgs ep_ctl{};
    ep_ctl.a[0] = epfd;
    ep_ctl.a[1] = 1;
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
struct GuestIovec {
    void* base;
    uint64_t len;
};
} // namespace

PS5DROID_TEST(mmap_requires_map_private) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    SyscallArgs a{};
    a.a[1] = 4096;
    a.a[2] = kProtRead | kProtWrite;
    a.a[3] = kMapAnonymous;
    a.a[4] = static_cast<uint64_t>(-1);
    DispatchResult r = d.invoke(kMmap, a);
    CHECK_MSG(r.ret == -1 && r.err == kEinval, "MAP_PRIVATE must be required");
}

PS5DROID_TEST(open_flags_are_linux_values) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

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

    SyscallArgs b{};
    b.a[0] = reinterpret_cast<uint64_t>("ps5droid_missing_file_definitely_absent");
    b.a[1] = kORdonly;
    DispatchResult r3 = d.invoke(kOpen, b);
    CHECK_MSG(r3.ret == -1 && r3.err == kEnoent, "missing file must ENOENT");

    SyscallArgs c{};
    c.a[0] = static_cast<uint64_t>(static_cast<int64_t>(5));
    c.a[1] = reinterpret_cast<uint64_t>(tmp.c_str());
    c.a[2] = kORdonly;
    DispatchResult r4 = d.invoke(kOpenat, c);
    CHECK_MSG(r4.ret == -1 && r4.err == kEbadf, "non-AT_FDCWD dirfd must EBADF");

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

    EpollEvent ev{1, 777};
    SyscallArgs add{};
    add.a[0] = epfd;
    add.a[1] = kEpollCtlAdd;
    add.a[2] = static_cast<uint64_t>(fd);
    add.a[3] = reinterpret_cast<uint64_t>(&ev);
    CHECK_MSG(d.invoke(kEpollCtl, add).ret == 0, "ADD failed");

    DispatchResult dup = d.invoke(kEpollCtl, add);
    CHECK_MSG(dup.ret == -1 && dup.err == kEexist, "duplicate ADD must EEXIST");

    SyscallArgs mod{};
    mod.a[0] = epfd;
    mod.a[1] = kEpollCtlMod;
    mod.a[2] = static_cast<uint64_t>(fd + 1);
    mod.a[3] = reinterpret_cast<uint64_t>(&ev);
    DispatchResult m = d.invoke(kEpollCtl, mod);
    CHECK_MSG(m.ret == -1 && m.err == kEnoent, "MOD of unregistered must ENOENT");

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

    EpollEvent ev{1, 4242};
    SyscallArgs add{};
    add.a[0] = epfd;
    add.a[1] = kEpollCtlAdd;
    add.a[2] = static_cast<uint64_t>(evfd);
    add.a[3] = reinterpret_cast<uint64_t>(&ev);
    CHECK_MSG(d.invoke(kEpollCtl, add).ret == 0, "ADD failed");

    EpollEvent out[2];
    SyscallArgs wait{};
    wait.a[0] = epfd;
    wait.a[1] = reinterpret_cast<uint64_t>(out);
    wait.a[2] = 2;
    wait.a[3] = 0;
    DispatchResult r = d.invoke(kEpollWait, wait);
    CHECK_MSG(r.ret == 0, "empty eventfd must not be reported readable");

    uint64_t one = 1;
    SyscallArgs wr{};
    wr.a[0] = static_cast<uint64_t>(evfd);
    wr.a[1] = reinterpret_cast<uint64_t>(&one);
    wr.a[2] = sizeof(one);
    CHECK_MSG(d.invoke(kWrite, wr).ret == static_cast<int64_t>(sizeof(one)), "eventfd write");

    r = d.invoke(kEpollWait, wait);
    CHECK_MSG(r.ret == 1 && out[0].data == 4242 && (out[0].events & 1) != 0,
              "ready eventfd must be reported with requested bits");

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

    char buf[4] = {0};
    SyscallArgs pr{};
    pr.a[0] = static_cast<uint64_t>(fd);
    pr.a[1] = reinterpret_cast<uint64_t>(buf);
    pr.a[2] = 3;
    pr.a[3] = 2;
    CHECK_MSG(d.invoke(kPread64, pr).ret == 3, "pread64 failed");
    CHECK_MSG(strncmp(buf, "CDE", 3) == 0, "pread64 read wrong bytes");

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
    seek.a[2] = 0;
    r = d.invoke(kLseek, seek);
    CHECK_MSG(r.ret == 0, "lseek failed");

    char back[8] = {0};
    SyscallArgs rd{};
    rd.a[0] = static_cast<uint64_t>(fd);
    rd.a[1] = reinterpret_cast<uint64_t>(back);
    rd.a[2] = 8;
    CHECK_MSG(d.invoke(kRead, rd).ret == 5 && strcmp(back, "ABCDE") == 0,
              "writev content mismatch");

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
