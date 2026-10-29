// PS5Droid - Guest File Descriptor Table and Epoll abstraction.
//
// Milestone 3 (extended in v1.0.2 & v1.0.3): in-process file descriptor table
// managing simulated guest fds (files, streams, epoll instances, eventfds, pipes)
// with Linux-semantics behavior:
//   * lowest-free fd allocation (like Linux alloc_fd),
//   * Linux x86_64 O_* flag values on open/openat (kOCreat = 0x40, ...),
//   * epoll_ctl EEXIST/ENOENT semantics and readiness-based epoll_wait,
//   * eventfd counters with EFD_SEMAPHORE and overflow (EINVAL) semantics,
//   * pread64 that does not move the shared file offset,
//   * in-process unidirectional pipes.
//
// Operations report a Linux errno (from linux_abi::LinuxErrno) on failure
// so the dispatch layer can surface the correct error to the guest.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "compat/linux_abi.h"

namespace ps5droid {
namespace compat {

enum class FdType {
    kFile,
    kStream,
    kEpoll,
    kEventFd,
    kPipe,
};

struct StatBuf {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime_sec;
    int64_t st_mtime_sec;
    int64_t st_ctime_sec;
};

struct EpollEvent {
    uint32_t events; // requested (ctl) / reported (wait) event mask
    uint64_t data;
};

class CompatFdTable;

class FdEntry {
  public:
    virtual ~FdEntry() = default;
    virtual FdType type() const = 0;

    // Reads/writes return the byte count, or -1 with *err set to a Linux
    // errno value. `offset >= 0` selects positioned I/O (pread-style) which
    // must not move the entry's shared file offset.
    virtual int64_t read(void* buf, size_t count, int64_t offset, int* err) = 0;
    virtual int64_t write(const void* buf, size_t count, int64_t offset, int* err) = 0;
    virtual int64_t lseek(int64_t offset, int whence, int* err) = 0;
    virtual bool fstat(StatBuf* statbuf) = 0;
    virtual void close() = 0;

    // Event mask currently reported to epoll (linux_abi::LinuxEpollEventBit).
    // Default: always ready for read and write (regular files/streams).
    virtual uint32_t readiness() const { return linux_abi::kEpollIn | linux_abi::kEpollOut; }

    int status_flags() const { return status_flags_; }
    void set_status_flags(int flags) { status_flags_ = flags; }

  protected:
    static void set_err(int* err, int value) {
        if (err != nullptr)
            *err = value;
    }

    int status_flags_ = 0;
};

class CompatFdTable {
  public:
    struct TableSlot {
        std::shared_ptr<FdEntry> entry;
        int fd_flags = 0; // e.g. Linux kFdCloexec
    };

    CompatFdTable();
    ~CompatFdTable();

    // Opens a file with LINUX x86_64 O_* flag values (linux_abi::LinuxOpenFlag).
    // Returns the new fd, or a negative Linux errno (-ENOENT, -EEXIST, ...).
    int open_file(const std::string& path, int flags, int mode);

    int create_epoll();
    // Returns the new fd, or -EINVAL for initval above the kernel ceiling.
    int create_eventfd(uint64_t initval, int flags);

    // Creates an in-process guest pipe.
    int create_pipe(int pipefds[2], int flags);

    // Returns 0, or a negative Linux errno (-EEXIST / -ENOENT / -EINVAL).
    int epoll_ctl(int epfd, int op, int fd, const EpollEvent* event);
    int epoll_wait(int epfd, EpollEvent* events, int maxevents, int timeout_ms);

    // All return -1 with *err set to the Linux errno on failure.
    int64_t read(int fd, void* buf, size_t count, int* err = nullptr);
    int64_t pread64(int fd, void* buf, size_t count, int64_t offset, int* err = nullptr);
    int64_t write(int fd, const void* buf, size_t count, int* err = nullptr);
    int64_t lseek(int fd, int64_t offset, int whence, int* err = nullptr);
    bool fstat(int fd, StatBuf* statbuf);

    int dup(int oldfd);
    int dup2(int oldfd, int newfd);
    int dup_minfd(int oldfd, int minfd, bool cloexec = false);
    int fcntl(int fd, int cmd, uint64_t arg);
    int close(int fd);

    std::shared_ptr<FdEntry> get(int fd);

  private:
    int allocate_fd_locked(int minfd = 0);
    int dup_minfd_locked(int oldfd, int minfd, bool cloexec);

    std::mutex mutex_;
    std::unordered_map<int, TableSlot> table_;
};

} // namespace compat
} // namespace ps5droid
