// PS5Droid - Linux syscall dispatch experiment (implementation).
#include "compat/syscall_dispatch.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "platform/platform.h"

#if !defined(PS5DROID_PLATFORM_PS5)
#include <sys/stat.h>
#include <unistd.h>
#else
#include <sys/stat.h>
#include <unistd.h>
// Orbis payloads: stat/lstat/access/unlink/mkdir/getcwd come from the
// ps5-payload-sdk libc (BSD VFS wrappers over the verified syscalls).
#endif

namespace ps5droid {
namespace compat {

namespace {

struct TimespecView { // Linux/Orbis layout, 8+8 bytes on x86_64
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct TimevalView { // struct timeval, 8+8 bytes on x86_64
    int64_t tv_sec;
    int64_t tv_usec;
};

// Guest iovec layout on x86_64: { void* base; size_t len; } (16 bytes).
struct IovecView {
    void* base;
    uint64_t len;
};

// Fills the guest StatBuf (fd_table.h layout) from a host struct stat.
void fill_stat_buf(const struct stat& host, StatBuf* out) {
    std::memset(out, 0, sizeof(*out));
    out->st_dev = static_cast<uint64_t>(host.st_dev);
    out->st_ino = static_cast<uint64_t>(host.st_ino);
    out->st_nlink = static_cast<uint64_t>(host.st_nlink);
    out->st_mode = static_cast<uint32_t>(host.st_mode);
    out->st_uid = static_cast<uint32_t>(host.st_uid);
    out->st_gid = static_cast<uint32_t>(host.st_gid);
    out->st_rdev = static_cast<uint64_t>(host.st_rdev);
    out->st_size = static_cast<int64_t>(host.st_size);
    out->st_blksize = static_cast<int64_t>(host.st_blksize);
    out->st_blocks = static_cast<int64_t>(host.st_blocks);
    out->st_atime_sec = static_cast<int64_t>(host.st_atime);
    out->st_mtime_sec = static_cast<int64_t>(host.st_mtime);
    out->st_ctime_sec = static_cast<int64_t>(host.st_ctime);
}

// Maps a host errno to the Linux errno with the same value (the ABI keeps
// BSD and Linux errno numbering compatible for the subset used here).
int host_errno_to_linux() {
    return errno; // NOLINT(concurrency-mt-unsafe): per-call translation
}

} // namespace

SyscallDispatch::SyscallDispatch(CompatMemory* memory, CompatFdTable* fd_table)
    : memory_(memory), fd_table_(fd_table ? fd_table : &default_fd_table_),
      context_id_(0x50534400ull | (platform::monotonic_ns() & 0xffffull)) {}

DispatchResult SyscallDispatch::ok(int64_t ret, const char* note) {
    return DispatchResult{ret, 0, true, note};
}

DispatchResult SyscallDispatch::fail(int err, const char* note) {
    return DispatchResult{-1, err, true, note};
}

DispatchResult SyscallDispatch::nosys(const char* note) {
    return DispatchResult{-1, linux_abi::kEnosys, false, note};
}

DispatchResult SyscallDispatch::invoke(int linux_nr, const SyscallArgs& args) {
    using namespace linux_abi;

    switch (linux_nr) {
    case kMmap: {
        uintptr_t hint = static_cast<uintptr_t>(args.a[0]);
        size_t len = static_cast<size_t>(args.a[1]);
        int prot = static_cast<int>(args.a[2]);
        int flags = static_cast<int>(args.a[3]);
        int fd = static_cast<int>(static_cast<int64_t>(args.a[4]));
        int64_t off = static_cast<int64_t>(args.a[5]);
        MapResult r = memory_->mmap(hint, len, prot, flags, fd, off);
        if (r.address == nullptr)
            return fail(r.linux_errno, r.note);
        return ok(reinterpret_cast<int64_t>(r.address), r.note);
    }

    case kMunmap: {
        uintptr_t addr = static_cast<uintptr_t>(args.a[0]);
        size_t len = static_cast<size_t>(args.a[1]);
        MapResult r = memory_->munmap(addr, len);
        if (r.address == nullptr && r.linux_errno != 0)
            return fail(r.linux_errno, r.note);
        return ok(0, r.note);
    }

    case kMprotect: {
        uintptr_t addr = static_cast<uintptr_t>(args.a[0]);
        size_t len = static_cast<size_t>(args.a[1]);
        int prot = static_cast<int>(args.a[2]);
        MapResult r = memory_->mprotect(addr, len, prot);
        if (r.address == nullptr && r.linux_errno != 0)
            return fail(r.linux_errno, r.note);
        return ok(0, r.note);
    }

    case kBrk: {
        uintptr_t req = static_cast<uintptr_t>(args.a[0]);
        BrkResult r = memory_->brk(req);
        return ok(static_cast<int64_t>(r.new_break), r.note);
    }

    case kOpen:
    case kOpenat: {
        if (linux_nr == kOpenat) {
            int dirfd = static_cast<int>(static_cast<int64_t>(args.a[0]));
            if (dirfd != kAtFdcwd)
                return fail(kEbadf, "only dirfd == AT_FDCWD is supported");
        }
        const char* path =
            reinterpret_cast<const char*>(linux_nr == kOpenat ? args.a[1] : args.a[0]);
        int flags = static_cast<int>(linux_nr == kOpenat ? args.a[2] : args.a[1]);
        int mode = static_cast<int>(linux_nr == kOpenat ? args.a[3] : args.a[2]);
        if (!path)
            return fail(kEfault, "path is null");
        int fd = fd_table_->open_file(path, flags, mode);
        if (fd < 0)
            return fail(-fd, "open failed with guest errno");
        return ok(fd, "guest fd allocated (Linux O_* flags)");
    }

    case kClose: {
        int fd = static_cast<int>(static_cast<int64_t>(args.a[0]));
        if (fd_table_->close(fd) != 0)
            return fail(kEbadf, "invalid fd");
        return ok(0, "fd closed");
    }

    case kRead: {
        int fd = static_cast<int>(static_cast<int64_t>(args.a[0]));
        void* buf = reinterpret_cast<void*>(args.a[1]);
        size_t count = static_cast<size_t>(args.a[2]);
        if (!buf)
            return fail(kEfault, "read buf is null");
        int err = 0;
        int64_t r = fd_table_->read(fd, buf, count, &err);
        if (r < 0)
            return fail(err != 0 ? err : kEbadf, "read failed");
        return ok(r, "fd read ok");
    }

    case kPread64: {
        int fd = static_cast<int>(static_cast<int64_t>(args.a[0]));
        void* buf = reinterpret_cast<void*>(args.a[1]);
        size_t count = static_cast<size_t>(args.a[2]);
        int64_t off = static_cast<int64_t>(args.a[3]);
        if (!buf)
            return fail(kEfault, "buf is null");
        if (off < 0)
            return fail(kEinval, "negative offset");
        int err = 0;
        int64_t r = fd_table_->pread64(fd, buf, count, off, &err);
        if (r < 0)
            return fail(err != 0 ? err : kEbadf, "pread64 failed");
        return ok(r, "pread64 ok");
    }

    case kWrite: {
        int fd = static_cast<int>(static_cast<int64_t>(args.a[0]));
        const void* buf = reinterpret_cast<const void*>(args.a[1]);
        size_t count = static_cast<size_t>(args.a[2]);
        if (buf == nullptr)
            return fail(kEfault, "write buf is null");
        int err = 0;
        int64_t n = fd_table_->write(fd, buf, count, &err);
        if (n < 0)
            return fail(err != 0 ? err : kEbadf, "write failed");
        return ok(n, "stream/fd write ok");
    }

    case kWritev: {
        int fd = static_cast<int>(static_cast<int64_t>(args.a[0]));
        const IovecView* iov = reinterpret_cast<const IovecView*>(args.a[1]);
        int iovcnt = static_cast<int>(static_cast<int64_t>(args.a[2]));
        if (iov == nullptr && iovcnt > 0)
            return fail(kEfault, "iovec array is null");
        if (iovcnt < 0)
            return fail(kEinval, "negative iovcnt");
        if (iovcnt > kIovMax)
            return fail(kEinval, "iovcnt exceeds IOV_MAX");
        int64_t total = 0;
        for (int i = 0; i < iovcnt; ++i) {
            if (iov[i].base == nullptr && iov[i].len > 0)
                return fail(kEfault, "iovec base is null");
            int err = 0;
            int64_t n = fd_table_->write(fd, iov[i].base, static_cast<size_t>(iov[i].len), &err);
            if (n < 0) {
                // Linux reports the bytes transferred before the failure.
                return fail(err != 0 ? err : kEbadf, "writev failed mid-vector");
            }
            total += n;
        }
        return ok(total, "writev ok (guest iovecs are host pointers)");
    }

    case kLseek: {
        int fd = static_cast<int>(static_cast<int64_t>(args.a[0]));
        int64_t off = static_cast<int64_t>(args.a[1]);
        int whence = static_cast<int>(args.a[2]);
        if (whence != 0 /*SEEK_SET*/ && whence != 1 /*SEEK_CUR*/ && whence != 2 /*SEEK_END*/)
            return fail(kEinval, "invalid whence");
        int err = 0;
        int64_t pos = fd_table_->lseek(fd, off, whence, &err);
        if (pos < 0)
            return fail(err != 0 ? err : kEbadf, "lseek failed");
        return ok(pos, "lseek ok");
    }

    case kFstat: {
        int fd = static_cast<int>(static_cast<int64_t>(args.a[0]));
        StatBuf* st = reinterpret_cast<StatBuf*>(args.a[1]);
        if (!st)
            return fail(kEfault, "statbuf is null");
        if (!fd_table_->fstat(fd, st))
            return fail(kEbadf, "fstat failed");
        return ok(0, "fstat ok");
    }

    case kStat:
    case kNewfstatat: {
        if (linux_nr == kNewfstatat) {
            int dirfd = static_cast<int>(static_cast<int64_t>(args.a[0]));
            if (dirfd != kAtFdcwd)
                return fail(kEbadf, "only dirfd == AT_FDCWD is supported");
            int flags = static_cast<int>(static_cast<int64_t>(args.a[3]));
            if ((flags & ~kAtSymlinkNofollow) != 0)
                return fail(kEinval, "unsupported newfstatat flags");
        }
        const char* path =
            reinterpret_cast<const char*>(linux_nr == kNewfstatat ? args.a[1] : args.a[0]);
        StatBuf* st = reinterpret_cast<StatBuf*>(linux_nr == kNewfstatat ? args.a[2] : args.a[1]);
        if (!path)
            return fail(kEfault, "path is null");
        if (!st)
            return fail(kEfault, "statbuf is null");
        struct stat host_st;
        int rc;
        if (linux_nr == kNewfstatat &&
            (static_cast<int>(static_cast<int64_t>(args.a[3])) & kAtSymlinkNofollow) != 0) {
            rc = ::lstat(path, &host_st);
        } else {
            rc = ::stat(path, &host_st);
        }
        if (rc != 0) {
            int he = host_errno_to_linux();
            return fail(he == 0 ? kEnoent : he, "path stat failed");
        }
        fill_stat_buf(host_st, st);
        return ok(0, "path stat ok (guest StatBuf layout)");
    }

    case kAccess: {
        const char* path = reinterpret_cast<const char*>(args.a[0]);
        int mode = static_cast<int>(args.a[1]);
        if (!path)
            return fail(kEfault, "path is null");
        if (mode != kFOk && mode != kROk && mode != kWOk && mode != kXOk && mode != (kROk | kWOk) &&
            mode != (kROk | kXOk) && mode != (kWOk | kXOk) && mode != (kROk | kWOk | kXOk))
            return fail(kEinval, "invalid access mode");
        if (::access(path, mode) != 0) {
            int he = host_errno_to_linux();
            return fail(he == 0 ? kEaccess : he, "access failed");
        }
        return ok(0, "access ok");
    }

    case kMkdir: {
        const char* path = reinterpret_cast<const char*>(args.a[0]);
        int mode = static_cast<int>(args.a[1]);
        if (!path)
            return fail(kEfault, "path is null");
        if (::mkdir(path, static_cast<mode_t>(mode)) != 0) {
            int he = host_errno_to_linux();
            return fail(he == 0 ? kEio : he, "mkdir failed");
        }
        return ok(0, "mkdir ok");
    }

    case kUnlink: {
        const char* path = reinterpret_cast<const char*>(args.a[0]);
        if (!path)
            return fail(kEfault, "path is null");
        if (::unlink(path) != 0) {
            int he = host_errno_to_linux();
            return fail(he == 0 ? kEnoent : he, "unlink failed");
        }
        return ok(0, "unlink ok");
    }

    case kGetcwd: {
        char* buf = reinterpret_cast<char*>(args.a[0]);
        size_t size = static_cast<size_t>(args.a[1]);
        if (buf == nullptr)
            return fail(kEfault, "buffer is null");
        if (size == 0)
            return fail(kEinval, "size must be positive");
        if (::getcwd(buf, size) == nullptr) {
            int he = host_errno_to_linux();
            return fail(he == kErange ? kErange : (he == 0 ? kErange : he),
                        "cwd does not fit the buffer");
        }
        return ok(static_cast<int64_t>(strlen(buf)), "host process cwd");
    }

    case kGettimeofday: {
        TimevalView* tv = reinterpret_cast<TimevalView*>(args.a[0]);
        if (tv == nullptr)
            return fail(kEfault, "timeval is null");
        uint64_t ns = platform::wallclock_ns();
        tv->tv_sec = static_cast<int64_t>(ns / 1000000000ull);
        tv->tv_usec = static_cast<int64_t>((ns % 1000000000ull) / 1000ull);
        return ok(0, "timeval read");
    }

    case kClockGetres: {
        int clk = static_cast<int>(static_cast<int64_t>(args.a[0]));
        TimespecView* ts = reinterpret_cast<TimespecView*>(args.a[1]);
        if (ts == nullptr)
            return fail(kEfault, "timespec is null");
        if (clk != kClockRealtime && clk != kClockMonotonic)
            return fail(kEinval, "unsupported clock id");
        ts->tv_sec = 0;
        ts->tv_nsec = 1; // nanosecond-resolution platform clocks
        return ok(0, "clock resolution");
    }

    case kGetrandom: {
        void* buf = reinterpret_cast<void*>(args.a[0]);
        size_t buflen = static_cast<size_t>(args.a[1]);
        unsigned int flags = static_cast<unsigned int>(args.a[2]);
        if (buf == nullptr && buflen > 0)
            return fail(kEfault, "buffer is null");
        if (buflen == 0)
            return ok(0, "zero-length request");
        if ((flags & ~(1u /*GRND_NONBLOCK*/ | 2u /*GRND_RANDOM*/)) != 0)
            return fail(kEinval, "unsupported getrandom flags");
        FILE* ur = fopen("/dev/urandom", "rb");
        if (ur == nullptr)
            return fail(kEagain, "entropy source unavailable");
        size_t got = fread(buf, 1, buflen, ur);
        bool io_err = ferror(ur) != 0;
        fclose(ur);
        if (got != buflen || io_err)
            return fail(kEagain, "entropy source short read");
        return ok(static_cast<int64_t>(got),
                  "entropy from host /dev/urandom (pool distinction not modeled)");
    }

    case kDup: {
        int oldfd = static_cast<int>(static_cast<int64_t>(args.a[0]));
        int newfd = fd_table_->dup(oldfd);
        if (newfd < 0)
            return fail(kEbadf, "dup failed");
        return ok(newfd, "dup ok");
    }

    case kDup2: {
        int oldfd = static_cast<int>(static_cast<int64_t>(args.a[0]));
        int newfd = static_cast<int>(static_cast<int64_t>(args.a[1]));
        int res = fd_table_->dup2(oldfd, newfd);
        if (res < 0)
            return fail(kEbadf, "dup2 failed");
        return ok(res, "dup2 ok");
    }

    case kEpollCreate1: {
        int fd = fd_table_->create_epoll();
        if (fd < 0)
            return fail(kEnomem, "epoll_create1 failed");
        return ok(fd, "epoll instance created");
    }

    case kEpollCtl: {
        int epfd = static_cast<int>(static_cast<int64_t>(args.a[0]));
        int op = static_cast<int>(args.a[1]);
        int target_fd = static_cast<int>(args.a[2]);
        const EpollEvent* event = reinterpret_cast<const EpollEvent*>(args.a[3]);
        int rc = fd_table_->epoll_ctl(epfd, op, target_fd, event);
        if (rc != 0) {
            int err = rc < 0 ? -rc : kEinval;
            const char* why = err == kEexist   ? "EPOLL_CTL_ADD on registered fd"
                              : err == kEnoent ? "fd not registered with this epoll"
                              : err == kEbadf  ? "not an epoll fd"
                                               : "epoll_ctl failed";
            return fail(err, why);
        }
        return ok(0, "epoll_ctl ok");
    }

    case kEpollWait: {
        int epfd = static_cast<int>(static_cast<int64_t>(args.a[0]));
        EpollEvent* events = reinterpret_cast<EpollEvent*>(args.a[1]);
        int maxevents = static_cast<int>(args.a[2]);
        int timeout = static_cast<int>(args.a[3]);
        int n = fd_table_->epoll_wait(epfd, events, maxevents, timeout);
        if (n < 0)
            return fail(kEinval, "epoll_wait failed");
        return ok(n, n > 0 ? "ready entries reported" : "timed out without readiness");
    }

    case kEventfd2: {
        uint64_t initval = args.a[0];
        int flags = static_cast<int>(args.a[1]);
        int fd = fd_table_->create_eventfd(initval, flags);
        if (fd < 0)
            return fail(fd == -kEinval ? kEinval : kEnomem, fd == -kEinval
                                                                ? "initval above counter ceiling"
                                                                : "eventfd creation failed");
        return ok(fd, "eventfd created");
    }

    case kGetpid:
        return ok(static_cast<int64_t>(context_id_), "single-process emulation context id");

    case kGettid:
        return ok(static_cast<int64_t>(platform::current_thread_id()), "platform thread id");

    case kClockGettime: {
        int clk = static_cast<int>(static_cast<int64_t>(args.a[0]));
        TimespecView* ts = reinterpret_cast<TimespecView*>(args.a[1]);
        if (ts == nullptr)
            return fail(kEfault, "timespec is null");
        uint64_t ns;
        if (clk == kClockRealtime) {
            ns = platform::wallclock_ns();
        } else if (clk == kClockMonotonic) {
            ns = platform::monotonic_ns();
        } else {
            return fail(kEinval, "unsupported clock id");
        }
        ts->tv_sec = static_cast<int64_t>(ns / 1000000000ull);
        ts->tv_nsec = static_cast<int64_t>(ns % 1000000000ull);
        return ok(0, "clock read");
    }

    case kNanosleep: {
        const TimespecView* req = reinterpret_cast<const TimespecView*>(args.a[0]);
        TimespecView* rem = reinterpret_cast<TimespecView*>(args.a[1]);
        if (req == nullptr)
            return fail(kEfault, "timespec is null");
        if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec > 999999999) {
            return fail(kEinval, "invalid timespec");
        }
        platform::sleep_ns(static_cast<uint64_t>(req->tv_sec) * 1000000000ull +
                           static_cast<uint64_t>(req->tv_nsec));
        if (rem != nullptr) {
            rem->tv_sec = 0;
            rem->tv_nsec = 0;
        }
        return ok(0, "slept");
    }

    case kSchedYield:
        platform::yield_thread();
        return ok(0, "yielded");

    case kFutex: {
        uint32_t* uaddr = reinterpret_cast<uint32_t*>(args.a[0]);
        uint32_t op = static_cast<uint32_t>(args.a[1]);
        uint32_t val = static_cast<uint32_t>(args.a[2]);
        if (uaddr == nullptr)
            return fail(kEfault, "uaddr is null");
        uint32_t base = op & ~kFutexPrivateFlag;
        bool priv = (op & kFutexPrivateFlag) != 0;
        if (base == kFutexWait && priv) {
            int r = platform::Futex().wait(uaddr, val);
            return ok(r, r == 0 ? "woke" : "EWOULDBLOCK");
        }
        if (base == kFutexWake && priv) {
            int r = platform::Futex().wake(uaddr, val);
            return ok(r, "waiters woken");
        }
        return fail(kEinval, "only FUTEX_WAIT/WAKE with PRIVATE flag supported");
    }

    case kExitGroup:
        stop_requested_ = true;
        return ok(0, "context stop requested");

    default:
        break;
    }

    // Known syscall: surface the table's static note (stable storage;
    // never return pointers to stack-local buffers).
    for (int i = 0; i < linux_abi::kSyscallTableSize; ++i) {
        if (linux_abi::kSyscallTable[i].number == linux_nr) {
            return nosys(linux_abi::kSyscallTable[i].note);
        }
    }
    return nosys("unknown Linux syscall number for x86_64");
}

std::vector<SyscallDispatch::MatrixEntry> SyscallDispatch::matrix() const {
    std::vector<MatrixEntry> out;
    for (int i = 0; i < linux_abi::kSyscallTableSize; ++i) {
        const auto& e = linux_abi::kSyscallTable[i];
        out.push_back(MatrixEntry{e.number, e.name, e.support, e.note});
    }
    return out;
}

} // namespace compat
} // namespace ps5droid
