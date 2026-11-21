// PS5Droid - v1.0.6 technical-debt cleanup syscall tests.
//
// Every syscall exercised here moved out of the UNSUPPORTED column in
// v1.0.6. The tests prove real behavior (or the documented partial
// semantics) rather than stubs.
#include "test_framework.h"

#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "compat/memory_compat.h"
#include "compat/syscall_dispatch.h"
#include "platform/platform.h"

using namespace ps5droid::compat;
using namespace ps5droid::linux_abi;

namespace {

struct GuestTimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

void* map_anon(SyscallDispatch* d, size_t len) {
    SyscallArgs a{};
    a.a[1] = len;
    a.a[2] = kProtRead | kProtWrite;
    a.a[3] = kMapPrivate | kMapAnonymous;
    a.a[4] = static_cast<uint64_t>(-1);
    DispatchResult r = d->invoke(kMmap, a);
    return r.ret > 0 ? reinterpret_cast<void*>(r.ret) : nullptr;
}

} // namespace

PS5DROID_TEST(mremap_and_madvise) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    uint8_t* p = static_cast<uint8_t*>(map_anon(&d, 4096));
    CHECK_MSG(p != nullptr, "initial mmap failed");
    memset(p, 0x5A, 4096);

    // Grow with MREMAP_MAYMOVE: contents preserved, address may move.
    SyscallArgs grow{};
    grow.a[0] = reinterpret_cast<uint64_t>(p);
    grow.a[1] = 4096;
    grow.a[2] = 8192;
    grow.a[3] = kMremapMaymove;
    DispatchResult g = d.invoke(kMremap, grow);
    CHECK_MSG(g.ret > 0, "mremap MAYMOVE grow failed");
    uint8_t* grown = reinterpret_cast<uint8_t*>(g.ret);
    CHECK_MSG(grown[0] == 0x5A && grown[4095] == 0x5A, "mremap grow lost data");
    memset(grown + 4096, 0xA5, 4096);

    // Grow without MAYMOVE is refused.
    SyscallArgs no_move{};
    no_move.a[0] = reinterpret_cast<uint64_t>(grown);
    no_move.a[1] = 8192;
    no_move.a[2] = 16384;
    no_move.a[3] = 0;
    DispatchResult nm = d.invoke(kMremap, no_move);
    CHECK_MSG(nm.ret == -1 && nm.err == kEnomem, "grow without MAYMOVE must ENOMEM");

    // Shrink in place.
    SyscallArgs shrink{};
    shrink.a[0] = reinterpret_cast<uint64_t>(grown);
    shrink.a[1] = 8192;
    shrink.a[2] = 4096;
    shrink.a[3] = 0;
    DispatchResult s = d.invoke(kMremap, shrink);
    CHECK_MSG(s.ret == static_cast<int64_t>(reinterpret_cast<uint64_t>(grown)),
              "shrink must keep the address");

    // MADV_DONTNEED zeroes the anonymous private range.
    SyscallArgs adv{};
    adv.a[0] = reinterpret_cast<uint64_t>(grown);
    adv.a[1] = 4096;
    adv.a[2] = kMadvDontneed;
    CHECK_MSG(d.invoke(kMadvise, adv).ret == 0, "MADV_DONTNEED failed");
    CHECK_MSG(grown[0] == 0 && grown[4095] == 0, "MADV_DONTNEED must zero pages");

    SyscallArgs normal{};
    normal.a[0] = reinterpret_cast<uint64_t>(grown);
    normal.a[1] = 4096;
    normal.a[2] = kMadvNormal;
    CHECK_MSG(d.invoke(kMadvise, normal).ret == 0, "MADV_NORMAL failed");

    SyscallArgs bad{};
    bad.a[0] = reinterpret_cast<uint64_t>(grown);
    bad.a[1] = 4096;
    bad.a[2] = 12345;
    DispatchResult br = d.invoke(kMadvise, bad);
    CHECK_MSG(br.ret == -1 && br.err == kEinval, "unknown advice must EINVAL");

    SyscallArgs unmap{};
    unmap.a[0] = reinterpret_cast<uint64_t>(grown);
    unmap.a[1] = 4096;
    CHECK_MSG(d.invoke(kMunmap, unmap).ret == 0, "munmap failed");
}

PS5DROID_TEST(chdir_tracks_guest_cwd) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    const std::string dir = "ps5droid_cwd_probe.dir";
    const std::string inner = dir + "/inner.tmp";
    ps5droid::platform::remove_file(inner);
    ps5droid::platform::make_dir(dir, 0755);

    char original[512] = {0};
    SyscallArgs cwd{};
    cwd.a[0] = reinterpret_cast<uint64_t>(original);
    cwd.a[1] = sizeof(original);
    CHECK_MSG(d.invoke(kGetcwd, cwd).ret > 0, "getcwd before chdir failed");

    SyscallArgs ch{};
    ch.a[0] = reinterpret_cast<uint64_t>(dir.c_str());
    CHECK_MSG(d.invoke(kChdir, ch).ret == 0, "chdir into temp dir failed");

    char after[512] = {0};
    cwd.a[0] = reinterpret_cast<uint64_t>(after);
    cwd.a[1] = sizeof(after);
    CHECK_MSG(d.invoke(kGetcwd, cwd).ret > 0, "getcwd after chdir failed");
    CHECK_MSG(std::string(after).find(dir) != std::string::npos,
              "guest cwd must include the new directory");

    // Relative open/stat resolve against the guest cwd.
    SyscallArgs open{};
    open.a[0] = reinterpret_cast<uint64_t>("inner.tmp");
    open.a[1] = kOCreat | kOTrunc | kORdwr;
    DispatchResult orr = d.invoke(kOpen, open);
    CHECK_MSG(orr.ret >= 3, "relative open failed");

    StatBuf st{};
    SyscallArgs stat_args{};
    stat_args.a[0] = reinterpret_cast<uint64_t>("inner.tmp");
    stat_args.a[1] = reinterpret_cast<uint64_t>(&st);
    CHECK_MSG(d.invoke(kStat, stat_args).ret == 0, "relative stat failed");
    CHECK_MSG(st.st_size == 0, "new file size must be zero");

    SyscallArgs close_args{};
    close_args.a[0] = static_cast<uint64_t>(orr.ret);
    d.invoke(kClose, close_args);

    // Missing target and non-directory target report the Linux errno.
    SyscallArgs missing{};
    missing.a[0] = reinterpret_cast<uint64_t>("no_such_dir_ps5droid");
    DispatchResult mr = d.invoke(kChdir, missing);
    CHECK_MSG(mr.ret == -1 && mr.err == kEnoent, "missing chdir target must ENOENT");

    SyscallArgs file_target{};
    file_target.a[0] = reinterpret_cast<uint64_t>("inner.tmp");
    DispatchResult fr = d.invoke(kChdir, file_target);
    CHECK_MSG(fr.ret == -1 && fr.err == kEnotdir, "file chdir target must ENOTDIR");

    // Return to the original directory with an absolute path.
    SyscallArgs back{};
    back.a[0] = reinterpret_cast<uint64_t>(original);
    CHECK_MSG(d.invoke(kChdir, back).ret == 0, "chdir back failed");

    ps5droid::platform::remove_file(inner);
    rmdir(dir.c_str());
}

PS5DROID_TEST(personality_arch_prctl_affinity_robust_wait_exit) {
    CompatMemory mem;
    SyscallDispatch d(&mem);

    SyscallArgs persona{};
    CHECK_MSG(d.invoke(kPersonality, persona).ret == kPerLinux,
              "personality must report PER_LINUX");
    persona.a[0] = 0xffffffffull;
    CHECK_MSG(d.invoke(kPersonality, persona).ret == kPerLinux, "personality query failed");
    persona.a[0] = 1;
    CHECK_MSG(d.invoke(kPersonality, persona).ret == -1, "unknown persona must fail");

    uint64_t fs_value = 0;
    SyscallArgs set_fs{};
    set_fs.a[0] = kArchSetFs;
    set_fs.a[1] = 0xdeadbeefull;
    CHECK_MSG(d.invoke(kArchPrctl, set_fs).ret == 0, "ARCH_SET_FS failed");
    SyscallArgs get_fs{};
    get_fs.a[0] = kArchGetFs;
    get_fs.a[1] = reinterpret_cast<uint64_t>(&fs_value);
    CHECK_MSG(d.invoke(kArchPrctl, get_fs).ret == 0, "ARCH_GET_FS failed");
    CHECK_MSG(fs_value == 0xdeadbeefull, "ARCH_GET_FS value mismatch");
    get_fs.a[0] = 999;
    CHECK_MSG(d.invoke(kArchPrctl, get_fs).ret == -1, "unknown arch_prctl must fail");

    uint64_t mask = 1;
    SyscallArgs set_aff{};
    set_aff.a[0] = 0;
    set_aff.a[1] = sizeof(mask);
    set_aff.a[2] = reinterpret_cast<uint64_t>(&mask);
    CHECK_MSG(d.invoke(kSchedSetaffinity, set_aff).ret == 0, "sched_setaffinity failed");

    uint64_t got_mask = 0;
    SyscallArgs get_aff{};
    get_aff.a[0] = 0;
    get_aff.a[1] = sizeof(got_mask);
    get_aff.a[2] = reinterpret_cast<uint64_t>(&got_mask);
    CHECK_MSG(d.invoke(kSchedGetaffinity, get_aff).ret == 0, "sched_getaffinity failed");
    CHECK_MSG(got_mask == 1, "affinity must report CPU 0");

    uint64_t empty_mask = 0;
    set_aff.a[2] = reinterpret_cast<uint64_t>(&empty_mask);
    CHECK_MSG(d.invoke(kSchedSetaffinity, set_aff).ret == -1, "empty affinity mask must fail");

    SyscallArgs robust{};
    robust.a[0] = 0x1234;
    robust.a[1] = 24;
    CHECK_MSG(d.invoke(kSetRobustList, robust).ret == 0, "set_robust_list failed");

    DispatchResult wr = d.invoke(kWait4, {});
    CHECK_MSG(wr.ret == -1 && wr.err == kEchild, "wait4 must ECHILD");

    CHECK_MSG(!d.stop_requested(), "context must start running");
    CHECK_MSG(d.invoke(kExit, {}).ret == 0, "exit must return 0");
    CHECK_MSG(d.stop_requested(), "exit must flag the context");
}

PS5DROID_TEST(flock_advisory_locks) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    const std::string path = "ps5droid_flock_probe.tmp";
    SyscallArgs open{};
    open.a[0] = reinterpret_cast<uint64_t>(path.c_str());
    open.a[1] = kOCreat | kOTrunc | kORdwr;
    int fd1 = static_cast<int>(d.invoke(kOpen, open).ret);
    int fd2 = static_cast<int>(d.invoke(kOpen, open).ret);
    CHECK_MSG(fd1 >= 3 && fd2 >= 3 && fd1 != fd2, "two opens failed");

    SyscallArgs lk{};
    lk.a[0] = static_cast<uint64_t>(fd1);
    lk.a[1] = kFlockEx | kFlockNb;
    CHECK_MSG(d.invoke(kFlock, lk).ret == 0, "exclusive lock failed");

    lk.a[0] = static_cast<uint64_t>(fd2);
    DispatchResult conflict = d.invoke(kFlock, lk);
    CHECK_MSG(conflict.ret == -1 && conflict.err == kEagain,
              "conflicting exclusive lock must EAGAIN");

    lk.a[1] = kFlockSh | kFlockNb;
    conflict = d.invoke(kFlock, lk);
    CHECK_MSG(conflict.ret == -1 && conflict.err == kEagain,
              "shared lock must conflict with exclusive");

    lk.a[0] = static_cast<uint64_t>(fd1);
    lk.a[1] = kFlockUn;
    CHECK_MSG(d.invoke(kFlock, lk).ret == 0, "unlock failed");

    lk.a[0] = static_cast<uint64_t>(fd2);
    lk.a[1] = kFlockSh | kFlockNb;
    CHECK_MSG(d.invoke(kFlock, lk).ret == 0, "shared lock after unlock failed");

    lk.a[0] = static_cast<uint64_t>(fd1);
    lk.a[1] = kFlockEx | kFlockNb;
    conflict = d.invoke(kFlock, lk);
    CHECK_MSG(conflict.ret == -1 && conflict.err == kEagain,
              "exclusive lock must conflict with shared");

    lk.a[0] = static_cast<uint64_t>(fd2);
    lk.a[1] = kFlockUn;
    CHECK_MSG(d.invoke(kFlock, lk).ret == 0, "shared unlock failed");

    lk.a[0] = static_cast<uint64_t>(fd1);
    lk.a[1] = kFlockEx | kFlockNb;
    CHECK_MSG(d.invoke(kFlock, lk).ret == 0, "exclusive lock after shared unlock failed");

    // dup shares the open file description: closing one fd keeps the lock
    // until the last descriptor is gone.
    SyscallArgs dup_args{};
    dup_args.a[0] = static_cast<uint64_t>(fd1);
    DispatchResult dup_res = d.invoke(kDup, dup_args);
    CHECK_MSG(dup_res.ret >= 3, "dup failed");
    int dup_fd = static_cast<int>(dup_res.ret);

    SyscallArgs cl{};
    cl.a[0] = static_cast<uint64_t>(fd1);
    d.invoke(kClose, cl);

    lk.a[0] = static_cast<uint64_t>(fd2);
    conflict = d.invoke(kFlock, lk);
    CHECK_MSG(conflict.ret == -1 && conflict.err == kEagain,
              "lock must survive closing one dup'ed descriptor");

    lk.a[0] = static_cast<uint64_t>(dup_fd);
    CHECK_MSG(d.invoke(kFlock, lk).ret == 0, "dup'ed descriptor shares the lock");

    cl.a[0] = static_cast<uint64_t>(dup_fd);
    d.invoke(kClose, cl);
    lk.a[0] = static_cast<uint64_t>(fd2);
    CHECK_MSG(d.invoke(kFlock, lk).ret == 0, "last close must release the lock");

    cl.a[0] = static_cast<uint64_t>(fd2);
    d.invoke(kClose, cl);
    ps5droid::platform::remove_file(path);
}

PS5DROID_TEST(memfd_and_fionread) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    SyscallArgs create{};
    create.a[0] = reinterpret_cast<uint64_t>("ps5droid-memfd");
    create.a[1] = kMemfdCloexec;
    DispatchResult cr = d.invoke(kMemfdCreate, create);
    CHECK_MSG(cr.ret >= 3, "memfd_create failed");
    int fd = static_cast<int>(cr.ret);

    const char* text = "memfd payload";
    SyscallArgs wr{};
    wr.a[0] = static_cast<uint64_t>(fd);
    wr.a[1] = reinterpret_cast<uint64_t>(text);
    wr.a[2] = strlen(text);
    CHECK_MSG(d.invoke(kWrite, wr).ret == static_cast<int64_t>(strlen(text)), "memfd write failed");

    SyscallArgs seek{};
    seek.a[0] = static_cast<uint64_t>(fd);
    seek.a[1] = 0;
    seek.a[2] = 0;
    CHECK_MSG(d.invoke(kLseek, seek).ret == 0, "memfd seek failed");

    int available = 0;
    SyscallArgs ioctl_args{};
    ioctl_args.a[0] = static_cast<uint64_t>(fd);
    ioctl_args.a[1] = kFionread;
    ioctl_args.a[2] = reinterpret_cast<uint64_t>(&available);
    CHECK_MSG(d.invoke(kIoctl, ioctl_args).ret == 0, "FIONREAD failed");
    CHECK_MSG(available == static_cast<int>(strlen(text)), "FIONREAD count mismatch");

    char buf[32] = {0};
    SyscallArgs rd{};
    rd.a[0] = static_cast<uint64_t>(fd);
    rd.a[1] = reinterpret_cast<uint64_t>(buf);
    rd.a[2] = sizeof(buf);
    CHECK_MSG(d.invoke(kRead, rd).ret == static_cast<int64_t>(strlen(text)), "memfd read failed");
    CHECK_MSG(strcmp(buf, text) == 0, "memfd data mismatch");

    StatBuf st{};
    SyscallArgs fst{};
    fst.a[0] = static_cast<uint64_t>(fd);
    fst.a[1] = reinterpret_cast<uint64_t>(&st);
    CHECK_MSG(d.invoke(kFstat, fst).ret == 0, "memfd fstat failed");
    CHECK_MSG(st.st_size == static_cast<int64_t>(strlen(text)), "memfd size mismatch");

    ioctl_args.a[1] = kTiocgwinsz;
    DispatchResult notty = d.invoke(kIoctl, ioctl_args);
    CHECK_MSG(notty.ret == -1 && notty.err == kEnotty, "unsupported ioctl must ENOTTY");

    create.a[1] = 0x4000;
    DispatchResult bad_flags = d.invoke(kMemfdCreate, create);
    CHECK_MSG(bad_flags.ret == -1 && bad_flags.err == kEinval, "unknown memfd flags must EINVAL");

    SyscallArgs cl{};
    cl.a[0] = static_cast<uint64_t>(fd);
    d.invoke(kClose, cl);
}

PS5DROID_TEST(getdents64_directory_stream) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    const std::string dir = "ps5droid_dents_probe.dir";
    ps5droid::platform::make_dir(dir, 0755);
    ps5droid::platform::write_file(dir + "/alpha", "a", 1);
    ps5droid::platform::write_file(dir + "/beta", "b", 1);

    SyscallArgs open{};
    open.a[0] = reinterpret_cast<uint64_t>(dir.c_str());
    open.a[1] = kORdonly | kODirectory;
    DispatchResult orr = d.invoke(kOpen, open);
    CHECK_MSG(orr.ret >= 3, "directory open failed");
    int fd = static_cast<int>(orr.ret);

    uint8_t buffer[512];
    SyscallArgs gd{};
    gd.a[0] = static_cast<uint64_t>(fd);
    gd.a[1] = reinterpret_cast<uint64_t>(buffer);
    gd.a[2] = sizeof(buffer);
    DispatchResult r = d.invoke(kGetdents64, gd);
    CHECK_MSG(r.ret > 0, "getdents64 returned no records");

    std::vector<std::string> names;
    size_t offset = 0;
    while (offset + 19 <= static_cast<size_t>(r.ret)) {
        uint16_t reclen = 0;
        memcpy(&reclen, buffer + offset + 16, sizeof(reclen));
        CHECK_MSG(reclen >= 19 && offset + reclen <= static_cast<size_t>(r.ret),
                  "invalid dirent64 record length");
        names.emplace_back(reinterpret_cast<const char*>(buffer + offset + 19));
        offset += reclen;
    }
    CHECK_MSG(names.size() == 4, "expected . .. alpha beta");
    CHECK_MSG(names[0] == "." && names[1] == "..", "dot entries must come first");
    CHECK_MSG(names[2] == "alpha" && names[3] == "beta", "directory names mismatch");

    DispatchResult eof = d.invoke(kGetdents64, gd);
    CHECK_MSG(eof.ret == 0, "second getdents64 must report EOF");

    // O_DIRECTORY on a regular file is ENOTDIR.
    const std::string file = dir + "/alpha";
    open.a[0] = reinterpret_cast<uint64_t>(file.c_str());
    DispatchResult notdir = d.invoke(kOpen, open);
    CHECK_MSG(notdir.ret == -1 && notdir.err == kEnotdir, "O_DIRECTORY on file must ENOTDIR");

    // getdents64 on a regular fd is ENOTDIR.
    open.a[0] = reinterpret_cast<uint64_t>(file.c_str());
    open.a[1] = kORdonly;
    int file_fd = static_cast<int>(d.invoke(kOpen, open).ret);
    gd.a[0] = static_cast<uint64_t>(file_fd);
    DispatchResult not_dir = d.invoke(kGetdents64, gd);
    CHECK_MSG(not_dir.ret == -1 && not_dir.err == kEnotdir,
              "getdents64 on regular file must ENOTDIR");

    SyscallArgs cl{};
    cl.a[0] = static_cast<uint64_t>(fd);
    d.invoke(kClose, cl);
    cl.a[0] = static_cast<uint64_t>(file_fd);
    d.invoke(kClose, cl);

    ps5droid::platform::remove_file(file);
    ps5droid::platform::remove_file(dir + "/beta");
    rmdir(dir.c_str());
}

PS5DROID_TEST(ppoll_readiness_and_nval) {
    CompatMemory mem;
    CompatFdTable fdt;
    SyscallDispatch d(&mem, &fdt);

    int pipefds[2] = {-1, -1};
    SyscallArgs pipe_args{};
    pipe_args.a[0] = reinterpret_cast<uint64_t>(pipefds);
    CHECK_MSG(d.invoke(kPipe2, pipe_args).ret == 0, "pipe2 failed");

    const char* msg = "ready";
    SyscallArgs wr{};
    wr.a[0] = static_cast<uint64_t>(pipefds[1]);
    wr.a[1] = reinterpret_cast<uint64_t>(msg);
    wr.a[2] = strlen(msg);
    CHECK_MSG(d.invoke(kWrite, wr).ret == static_cast<int64_t>(strlen(msg)), "pipe write failed");

    PollFd pollfd{};
    pollfd.fd = pipefds[0];
    pollfd.events = kPollIn;
    GuestTimespec zero{0, 0};
    SyscallArgs pp{};
    pp.a[0] = reinterpret_cast<uint64_t>(&pollfd);
    pp.a[1] = 1;
    pp.a[2] = reinterpret_cast<uint64_t>(&zero);
    DispatchResult r = d.invoke(kPpoll, pp);
    CHECK_MSG(r.ret == 1 && pollfd.revents == kPollIn, "ppoll must report POLLIN");

    // Invalid fd is reported as POLLNVAL.
    PollFd bad{};
    bad.fd = 999;
    bad.events = kPollIn;
    pp.a[0] = reinterpret_cast<uint64_t>(&bad);
    r = d.invoke(kPpoll, pp);
    CHECK_MSG(r.ret == 1 && bad.revents == kPollNval, "invalid fd must report POLLNVAL");

    // Drain the pipe and wait briefly: no readiness, timeout returns 0.
    char drain[16];
    SyscallArgs rd{};
    rd.a[0] = static_cast<uint64_t>(pipefds[0]);
    rd.a[1] = reinterpret_cast<uint64_t>(drain);
    rd.a[2] = sizeof(drain);
    d.invoke(kRead, rd);

    PollFd empty{};
    empty.fd = pipefds[0];
    empty.events = kPollIn;
    GuestTimespec short_wait{0, 2000000}; // 2 ms
    pp.a[0] = reinterpret_cast<uint64_t>(&empty);
    pp.a[2] = reinterpret_cast<uint64_t>(&short_wait);
    r = d.invoke(kPpoll, pp);
    CHECK_MSG(r.ret == 0 && empty.revents == 0, "ppoll must time out with no readiness");

    SyscallArgs cl{};
    cl.a[0] = static_cast<uint64_t>(pipefds[0]);
    d.invoke(kClose, cl);
    cl.a[0] = static_cast<uint64_t>(pipefds[1]);
    d.invoke(kClose, cl);
}
