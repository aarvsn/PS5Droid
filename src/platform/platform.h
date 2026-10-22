// PS5Droid - platform abstraction layer.
//
// This header declares the small, portable set of platform services the
// runtime needs: logging, time, memory mapping, threads/synchronization and
// filesystem access. Two backends implement it:
//
//   * POSIX backend  - Linux/macOS development hosts (and CI).
//   * PS5 backend    - Orbis OS payloads built with ps5-payload-sdk
//                      (https://github.com/ps5-payload-dev/sdk).
//
// The PS5 backend is only compiled when the ps5-payload-sdk toolchain
// (toolchain/prospero.cmake in the release zip; host/toolchain/ in a
// source checkout) drives the build. See docs/development.md and
// docs/platform-constraints.md for what has been verified on which backend.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps5droid {
namespace platform {

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

enum class LogLevel : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

using LogSink = void (*)(void* user, LogLevel level, const char* tag, const char* message);

void set_log_sink(LogSink sink, void* user);
void log(LogLevel level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

uint64_t monotonic_ns();
uint64_t wallclock_ns();
void sleep_ns(uint64_t ns);
void yield_thread();

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

// Page granularity of the platform's virtual memory allocator.
size_t page_size();

// Maps `size` bytes of page-aligned, zero-filled, read/write memory.
// Returns nullptr on failure. Must be released with unmap().
void* map_anonymous(size_t size);

// Releases a mapping previously returned by map_anonymous().
bool unmap(void* address, size_t size);

// Adjusts read/write protection of an existing mapping. Best effort: the
// result reflects the platform call, callers must not assume exec pages are
// grantable (see docs/platform-constraints.md).
bool protect_memory(void* address, size_t size, bool readable, bool writable);

// ---------------------------------------------------------------------------
// Threads and synchronization
// ---------------------------------------------------------------------------

uint32_t current_thread_id();

class Thread {
  public:
    using Fn = void (*)(void* ctx);
    Thread() = default;
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    bool start(Fn fn, void* ctx);
    // Joins the thread. Returns false if the thread was never started or was
    // already joined.
    bool join();
    bool started() const { return started_; }

  private:
    bool started_ = false;
    bool joined_ = false;
    void* handle_ = nullptr;
    void* ctx_ = nullptr;
    Fn fn_ = nullptr;
};

class Mutex {
  public:
    Mutex();
    ~Mutex();
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock();
    void unlock();

  private:
    friend class CondVar;
    struct Impl;
    Impl* impl_;
};

class CondVar {
  public:
    CondVar();
    ~CondVar();
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

    void wait(Mutex& mutex);
    void notify_one();
    void notify_all();

  private:
    struct Impl;
    Impl* impl_;
};

// Futex-like wait/wake primitive keyed on a user-supplied 32-bit word. This
// implements the semantics the Linux compat layer needs (see
// compat/syscall_dispatch.cpp): waiters sleep while *addr == expected.
//
// Both backends currently implement this on top of mutex/condvar. A PS5 probe
// against _umtx_op (FreeBSD umtx) is planned but NOT wired in yet; see
// docs/platform-constraints.md, "IPC / synchronization".
class Futex {
  public:
    Futex();
    ~Futex();
    Futex(const Futex&) = delete;
    Futex& operator=(const Futex&) = delete;

    // Returns 0 when woken by futex_wake(); returns 1 immediately when
    // *addr != expected (EWOULDBLOCK semantics).
    int wait(uint32_t* addr, uint32_t expected);

    // Wakes at most `count` waiters; returns the number woken.
    int wake(uint32_t* addr, uint32_t count);

  private:
    struct Impl;
    Impl* impl_;
};

// ---------------------------------------------------------------------------
// Filesystem
// ---------------------------------------------------------------------------

bool read_file(const std::string& path, std::string* out);
bool write_file(const std::string& path, const void* data, size_t len);
bool file_exists(const std::string& path);
bool remove_file(const std::string& path);
bool make_dir(const std::string& path, int mode);
std::vector<std::string> list_dir(const std::string& path);

// ---------------------------------------------------------------------------
// Backend identity
// ---------------------------------------------------------------------------

// "posix" on development hosts, "ps5" inside Orbis payloads.
const char* backend_name();

} // namespace platform
} // namespace ps5droid
