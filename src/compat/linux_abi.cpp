// PS5Droid - Linux ABI tables (see linux_abi.h for sources and rules).
#include "compat/linux_abi.h"

#include <cstddef>

namespace ps5droid {
namespace linux_abi {

const SyscallInfo kSyscallTable[] = {
    // --- emulated, real in-process behavior -------------------------------
    {kWrite, "write", kEmulated, "fd 1/2 passthrough to the process streams"},
    {kMmap, "mmap", kEmulated, "anonymous MAP_PRIVATE mappings only (arena-backed)"},
    {kMunmap, "munmap", kPartial,
     "whole-mapping release only; partial interior ranges return EINVAL"},
    {kMprotect, "mprotect", kPartial,
     "R/W adjustments honored; PROT_EXEC is accepted but not granted "
     "(no JIT path yet)"},
    {kBrk, "brk", kEmulated, "heap over an internal anonymous arena"},
    {kSchedYield, "sched_yield", kEmulated, "maps to platform yield"},
    {kNanosleep, "nanosleep", kEmulated, "maps to platform sleep"},
    {kGetpid, "getpid", kEmulated, "single-process model: returns the emulation context id"},
    {kGettid, "gettid", kEmulated, "maps to the platform thread id"},
    {kClockGettime, "clock_gettime", kEmulated,
     "CLOCK_MONOTONIC / CLOCK_REALTIME from platform clocks"},
    {kFutex, "futex", kPartial, "FUTEX_WAIT/WAKE with PRIVATE flag only, registry-based"},
    {kExitGroup, "exit_group", kEmulated, "flags the emulation context as stopping"},

    // --- guest fd table & stream / file operations ------------------------
    {kRead, "read", kEmulated, "reads from simulated guest fds"},
    {kOpen, "open", kEmulated,
     "opens files into in-process guest fd table (Linux O_* flag values)"},
    {kClose, "close", kEmulated, "closes guest fds"},
    {kFstat, "fstat", kEmulated, "fstat on guest fds"},
    {kLseek, "lseek", kEmulated, "lseek on simulated guest fds"},
    {kPread64, "pread64", kEmulated, "positioned read on guest fds; does not move the offset"},
    {kDup, "dup", kEmulated, "duplicates guest fds to the lowest free slot"},
    {kDup2, "dup2", kEmulated, "duplicates guest fds to target index"},
    {kEpollWait, "epoll_wait", kEmulated,
     "readiness-based wait over in-process epoll instances (eventfd counters)"},
    {kEpollCtl, "epoll_ctl", kEmulated,
     "ADD/DEL/MOD with Linux EEXIST/ENOENT semantics on in-process instances"},
    {kOpenat, "openat", kEmulated,
     "openat routed to guest fd table; only dirfd == AT_FDCWD is accepted"},
    {kEventfd2, "eventfd2", kEmulated,
     "eventfd counter with EFD_SEMAPHORE/overflow semantics in fd table"},
    {kEpollCreate1, "epoll_create1", kEmulated, "creates in-process epoll instance in fd table"},
    {kWritev, "writev", kEmulated,
     "writes guest iovecs to guest fds (iovec pointers are host pointers)"},

    // --- path / filesystem family (host stat/stdio backed) -----------------
    {kStat, "stat", kEmulated, "guest path stat via host stat(2); StatBuf is the guest layout"},
    {kAccess, "access", kEmulated, "F_OK/R_OK/W_OK/X_OK via host access(2)"},
    {kGetcwd, "getcwd", kEmulated, "returns the host process cwd (single-process model)"},
    {kMkdir, "mkdir", kEmulated, "mkdir via host mkdir(2); mode honored"},
    {kUnlink, "unlink", kEmulated, "unlink via host unlink(2)"},
    {kGettimeofday, "gettimeofday", kEmulated, "host realtime clock, timeval layout"},
    {kClockGetres, "clock_getres", kEmulated, "REALTIME/MONOTONIC resolution reported as 1 ns"},
    {kNewfstatat, "newfstatat", kEmulated, "AT_FDCWD only; AT_SYMLINK_NOFOLLOW honored via lstat"},
    {kGetrandom, "getrandom", kPartial,
     "entropy from the host /dev/urandom; GRND_RANDOM pool distinction not "
     "modeled; Orbis availability unverified [ASSUMPTION]"},

    // --- honestly unsupported; these are the hard blockers -----------------
    {kRtSigaction, "rt_sigaction", kUnsupported, "Linux signal ABI not implemented on Orbis"},
    {kRtSigprocmask, "rt_sigprocmask", kUnsupported, "Linux signal ABI not implemented on Orbis"},
    {kIoctl, "ioctl", kUnsupported, "Linux ioctl subsys vary wildly; none wired"},
    {kMremap, "mremap", kUnsupported, "not implemented"},
    {kMadvise, "madvise", kUnsupported, "not implemented"},
    {kSocket, "socket", kUnsupported, "BSD socket domains/families differ from Linux; not wired"},
    {kConnect, "connect", kUnsupported, "socket layer not implemented"},
    {kAccept, "accept", kUnsupported, "socket layer not implemented"},
    {kBind, "bind", kUnsupported, "socket layer not implemented"},
    {kListen, "listen", kUnsupported, "socket layer not implemented"},
    {kClone, "clone", kUnsupported,
     "HARD BLOCKER: no process/thread creation via Linux clone() on Orbis"},
    {kFork, "fork", kUnsupported, "HARD BLOCKER: no process creation API for Orbis payloads"},
    {kVfork, "vfork", kUnsupported, "HARD BLOCKER: see fork"},
    {kExecve, "execve", kUnsupported, "HARD BLOCKER: no ELF exec loader exposed to payloads"},
    {kExit, "exit", kUnsupported, "thread exit path not implemented"},
    {kWait4, "wait4", kUnsupported, "no child processes exist in the emulation model"},
    {kKill, "kill", kUnsupported, "no signal delivery between processes"},
    {kUname, "uname", kUnsupported, "would require synthesizing a fake kernel identity; refused"},
    {kFcntl, "fcntl", kUnsupported, "guest fd flag storage not implemented"},
    {kFlock, "flock", kUnsupported, "advisory locks not implemented"},
    {kFsync, "fsync", kUnsupported, "write-through is not guaranteed in-process"},
    {kChdir, "chdir", kUnsupported, "would change the host process cwd; side-effectful, refused"},
    {kReadlink, "readlink", kUnsupported, "guest symlink layer not implemented"},
    {kGetuid, "getuid", kUnsupported, "identity model undecided; refusing to fake a uid"},
    {kGetgid, "getgid", kUnsupported, "identity model undecided"},
    {kGeteuid, "geteuid", kUnsupported, "identity model undecided"},
    {kGetppid, "getppid", kUnsupported, "no parent in emulation model"},
    {kSigaltstack, "sigaltstack", kUnsupported, "signal ABI not implemented"},
    {kPersonality, "personality", kUnsupported, "not implemented"},
    {kPrctl, "prctl", kUnsupported, "bionic relies on prctl during init; not implemented"},
    {kArchPrctl, "arch_prctl", kUnsupported, "not implemented"},
    {kSchedSetaffinity, "sched_setaffinity", kUnsupported, "not implemented"},
    {kSchedGetaffinity, "sched_getaffinity", kUnsupported, "not implemented"},
    {kGetdents64, "getdents64", kUnsupported,
     "directory streams not in the fd table; Linux dirent layout not emitted"},
    {kSetTidAddress, "set_tid_address", kUnsupported, "not implemented"},
    {kClockNanosleep, "clock_nanosleep", kUnsupported, "not implemented"},
    {kTgkill, "tgkill", kUnsupported, "signal ABI not implemented"},
    {kUnlinkat, "unlinkat", kUnsupported, "dirfd-relative removal not wired (AT_FDCWD only paths)"},
    {kPpoll, "ppoll", kUnsupported, "poll layer not implemented"},
    {kSetRobustList, "set_robust_list", kUnsupported, "not implemented"},
    {kDup3, "dup3", kUnsupported, "guest fd table not implemented"},
    {kPipe2, "pipe2", kUnsupported, "no guest fd table"},
    {kInotifyInit1, "inotify_init1", kUnsupported, "not implemented"},
    {kMemfdCreate, "memfd_create", kUnsupported,
     "Linux memfd has no Orbis equivalent; unimplemented"},
};

const int kSyscallTableSize = static_cast<int>(sizeof(kSyscallTable) / sizeof(kSyscallTable[0]));

const char* syscall_name(int number) {
    for (int i = 0; i < kSyscallTableSize; ++i) {
        if (kSyscallTable[i].number == number && kSyscallTable[i].name) {
            return kSyscallTable[i].name;
        }
    }
    return nullptr;
}

const FreebsdMapping kFreebsdVerified[] = {
    {kRead, 3, "ps5-payload-sdk crt/syscall.h SYS_read"},
    {kWrite, 4, "ps5-payload-sdk crt/syscall.h SYS_write"},
    {kOpen, 5, "ps5-payload-sdk crt/syscall.h SYS_open"},
    {kClose, 6, "ps5-payload-sdk crt/syscall.h SYS_close"},
    {kGetpid, 20, "ps5-payload-sdk crt/syscall.h SYS_getpid"},
    {kAccess, 33, "ps5-payload-sdk crt/syscall.h SYS_access"},
    {kMunmap, 73, "ps5-payload-sdk crt/syscall.h SYS_munmap"},
    {kMprotect, 74, "ps5-payload-sdk crt/syscall.h SYS_mprotect"},
    {kMmap, 477, "ps5-payload-sdk crt/syscall.h SYS_mmap"},
    {kLseek, 478, "ps5-payload-sdk crt/syscall.h SYS_lseek"},
};

const int kFreebsdVerifiedSize =
    static_cast<int>(sizeof(kFreebsdVerified) / sizeof(kFreebsdVerified[0]));

const FreebsdMapping* freebsd_mapping(int linux_nr) {
    for (int i = 0; i < kFreebsdVerifiedSize; ++i) {
        if (kFreebsdVerified[i].linux_nr == linux_nr) {
            return &kFreebsdVerified[i];
        }
    }
    return nullptr;
}

} // namespace linux_abi
} // namespace ps5droid
