// PS5Droid - Linux ABI constants and translation tables.
//
// SCOPE AND SOURCES (verified):
//
//   * Linux x86_64 syscall numbers below are the well-known public UAPI
//     numbers for the architecture Android/bionic targets (arch/x86/entry/
//     syscalls/syscall_64.tbl in the Linux source tree). Constant values are
//     facts of the ABI and are documented here without copying kernel code.
//   * FreeBSD/Orbis syscall numbers in kFreebsdSyscall are the subset
//     VERIFIED by reading ps5-payload-dev/sdk crt/syscall.h in the SDK
//     source tree (SYS_read=3, SYS_write=4, SYS_open=5, SYS_close=6,
//     SYS_getpid=20, SYS_access=33, SYS_munmap=73, SYS_mprotect=74,
//     SYS_setsockopt=105, SYS_stat=188, SYS_sysctl=202, SYS___getcwd=326,
//     SYS_mmap=477, SYS_lseek=478, SYS_mdbg_call=573, SYS_sprx_dlsym=591,
//     SYS_sprx_load=594, SYS_sprx_unload=595, SYS_randomized_path=602,
//     SYS_dynlib_get_obj_member=649).
//
// The two tables together demonstrate the central platform constraint: a
// Linux x86_64 binary issues raw `syscall` instructions with Linux numbers
// (e.g. mmap=9), while Orbis OS expects FreeBSD-amd64 numbers (mmap=477).
// Direct execution of Linux binaries without ABI translation is therefore
// impossible; translation tables like the ones here are what a compat layer
// must provide. See docs/platform-constraints.md.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ps5droid {
namespace linux_abi {

// ---------------------------------------------------------------------------
// Linux errno values used by bionic (subset relevant to the dispatch layer).
// ---------------------------------------------------------------------------

enum LinuxErrno : int {
    kEperm = 1,
    kEnoent = 2,
    kEintr = 4,
    kEio = 5,
    kEbadf = 9,
    kEagain = 11,
    kEnomem = 12,
    kEaccess = 13,
    kEfault = 14,
    kEbusy = 16,
    kEexist = 17,
    kEnodev = 19,
    kEinval = 22,
    kEnfile = 23,
    kEmfile = 24,
    kEnospc = 28,
    kEpipe = 32,
    kErange = 34,
    kEnosys = 38,
    kEtimedout = 110,
};

// ESPIPE (29) — identical value on Linux and FreeBSD; the seeks on
// pipes/streams must report it instead of EPIPE when a guest seeks a
// non-seekable descriptor.
constexpr int kEspipe = 29;

// ---------------------------------------------------------------------------
// mmap / protection / clock constants (Linux x86_64 UAPI values).
// ---------------------------------------------------------------------------

enum LinuxMmapFlags : int {
    kMapShared = 0x01,
    kMapPrivate = 0x02,
    kMapFixed = 0x10,
    kMapAnonymous = 0x20,
};

enum LinuxProt : int {
    kProtRead = 0x1,
    kProtWrite = 0x2,
    kProtExec = 0x4,
};

enum LinuxClockId : int {
    kClockRealtime = 0,
    kClockMonotonic = 1,
};

// Linux struct timespec layout on x86_64 (time_t=8B, long=8B). Orbis (a
// FreeBSD descendant) uses the same layout, which keeps the clock path
// translation-free.
static constexpr size_t kTimespecSize = 16;

// ---------------------------------------------------------------------------
// Futex operations (Linux UAPI values used by bionic).
// ---------------------------------------------------------------------------

enum LinuxFutexOp : uint32_t {
    kFutexWait = 0,
    kFutexWake = 1,
    kFutexPrivateFlag = 128,
};

// ---------------------------------------------------------------------------
// Linux x86_64 open(2) / openat(2) flag values (UAPI). These are NOT the
// host's O_* values: Linux x86_64 uses O_CREAT=0x40 / O_TRUNC=0x200 /
// O_APPEND=0x400, which differs from e.g. Darwin where O_CREAT=0x200. The
// compat layer must translate with THESE values because the guest ABI is
// Linux (bionic).
// ---------------------------------------------------------------------------

enum LinuxOpenFlag : int {
    kOAccmode = 0x3,
    kORdonly = 0x0,
    kOWronly = 0x1,
    kORdwr = 0x2,
    kOCreat = 0x40,     // 0100
    kOExcl = 0x80,      // 0200
    kOTrunc = 0x200,    // 01000
    kOAppend = 0x400,   // 02000
    kONonblock = 0x800, // 020000
};

// openat/newfstatat/unlinkat dirfd special values and flags (Linux x86_64 UAPI).
enum LinuxAtFlag : int {
    kAtFdcwd = -100,
    kAtSymlinkNofollow = 0x100,
    kAtRemovedir = 0x200,
};

// fcntl(2) commands and descriptor flags (Linux x86_64 UAPI).
enum LinuxFcntlCmd : int {
    kFDupfd = 0,
    kFGetfd = 1,
    kFSetfd = 2,
    kFGetfl = 3,
    kFSetfl = 4,
    kFDupfdCloexec = 1030,
};

enum LinuxFdFlag : int {
    kFdCloexec = 1,
};

// eventfd2 flags (Linux x86_64 UAPI).
enum LinuxEventfdFlag : int {
    kEfdSemaphore = 0x1,
    kEfdNonblock = 0x800,
    kEfdCloexec = 0x80000,
};

// epoll_ctl operations and the event bits the in-process poller reports.
enum LinuxEpollOp : int {
    kEpollCtlAdd = 1,
    kEpollCtlDel = 2,
    kEpollCtlMod = 3,
};

enum LinuxEpollEventBit : uint32_t {
    kEpollIn = 0x1,
    kEpollOut = 0x4,
};

// access(2) mode values (POSIX-identical on both ABIs, kept for clarity).
enum LinuxAccessMode : int {
    kFOk = 0,
    kROk = 4,
    kWOk = 2,
    kXOk = 1,
};

// writev limit (Linux x86_64 IOV_MAX).
constexpr int kIovMax = 1024;

// eventfd counter ceiling (kernel refuses writes past this value).
constexpr uint64_t kEventfdMax = 0xfffffffffffffffeull;

// ---------------------------------------------------------------------------
// Curated Linux x86_64 syscall table.
//
// This is the working set the compat dispatch considers. It is deliberately
// small: every entry must have a defensible support classification. Do not
// add speculative entries.
// ---------------------------------------------------------------------------

enum LinuxSyscall : int {
    kRead = 0,
    kWrite = 1,
    kOpen = 2,
    kClose = 3,
    kStat = 4,
    kFstat = 5,
    kLseek = 8,
    kMmap = 9,
    kMprotect = 10,
    kMunmap = 11,
    kBrk = 12,
    kRtSigaction = 13,
    kRtSigprocmask = 14,
    kIoctl = 16,
    kPread64 = 17,
    kWritev = 20,
    kAccess = 21,
    kSchedYield = 24,
    kMremap = 25,
    kMadvise = 28,
    kDup = 32,
    kDup2 = 33,
    kNanosleep = 35,
    kGetpid = 39,
    kSocket = 41,
    kConnect = 42,
    kAccept = 43,
    kBind = 49,
    kListen = 50,
    kClone = 56,
    kFork = 57,
    kVfork = 58,
    kExecve = 59,
    kExit = 60,
    kWait4 = 61,
    kKill = 62,
    kUname = 63,
    kFcntl = 72,
    kFlock = 73,
    kFsync = 74,
    kGetcwd = 79,
    kChdir = 80,
    kMkdir = 83,
    kUnlink = 87,
    kReadlink = 89,
    kGettimeofday = 96,
    kGetuid = 102,
    kGetgid = 104,
    kGeteuid = 107,
    kGetegid = 108,
    kGetppid = 110,
    kSigaltstack = 132,
    kPersonality = 135,
    kPrctl = 157,
    kArchPrctl = 158,
    kFutex = 202,
    kSchedSetaffinity = 203,
    kSchedGetaffinity = 204,
    kGettid = 186,
    kGetdents64 = 217,
    kSetTidAddress = 218,
    kClockGettime = 228,
    kClockGetres = 229,
    kClockNanosleep = 230,
    kExitGroup = 231,
    kEpollWait = 232,
    kEpollCtl = 233,
    kTgkill = 234,
    kOpenat = 257,
    kNewfstatat = 262,
    kUnlinkat = 263,
    kPpoll = 271,
    kSetRobustList = 272,
    kEventfd2 = 290,
    kEpollCreate1 = 291,
    kDup3 = 292,
    kPipe2 = 293,
    kInotifyInit1 = 294,
    kMemfdCreate = 319,
    kGetrandom = 318,
};

// Support classification used by the compat matrix and the status reports.
enum Support {
    kEmulated,    // fully implemented in-process with real behavior
    kPartial,     // implemented with documented restrictions
    kUnsupported, // not implemented; dispatch returns -ENOSYS
};

struct SyscallInfo {
    int number;
    const char* name;
    Support support;
    const char* note;
};

// The table, ordered by syscall number. Defined in linux_abi.cpp.
extern const SyscallInfo kSyscallTable[];
extern const int kSyscallTableSize;

const char* syscall_name(int number);

// ---------------------------------------------------------------------------
// Linux -> FreeBSD/Orbis syscall number translation (verified subset only).
// Entries absent from this table have NOT been verified on Orbis and must
// not be assumed. Source: ps5-payload-dev/sdk crt/syscall.h.
// ---------------------------------------------------------------------------

struct FreebsdMapping {
    int linux_nr;
    int freebsd_nr;
    const char* note;
};

extern const FreebsdMapping kFreebsdVerified[];
extern const int kFreebsdVerifiedSize;

// Returns the verified FreeBSD number for a Linux syscall, or nullptr when
// unverified (callers must not guess).
const FreebsdMapping* freebsd_mapping(int linux_nr);

} // namespace linux_abi
} // namespace ps5droid
