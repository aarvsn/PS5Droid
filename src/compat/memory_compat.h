// PS5Droid - Linux memory semantics emulation (brk + anonymous mmap).
//
// This module provides the memory behavior the Linux ABI experiment needs:
//   * Linux brk() semantics over a fixed heap arena,
//   * anonymous MAP_PRIVATE mmap()/munmap()/mprotect() with Linux flag
//     validation.
//
// Everything here operates on HOST process memory (the "guest address
// space" IS the host address space in this in-process experiment; there is
// no address translation or isolation yet). This scope limitation is
// documented in docs/architecture.md and docs/platform-constraints.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "compat/linux_abi.h"

namespace ps5droid {
namespace compat {

struct MapResult {
    void* address;    // nullptr on failure
    int linux_errno;  // linux_abi::LinuxErrno value on failure
    const char* note; // human-readable note (never nullptr)
};

struct BrkResult {
    uintptr_t new_break; // resulting program break (Linux semantics)
    const char* note;
};

class CompatMemory {
  public:
    CompatMemory();
    ~CompatMemory();
    CompatMemory(const CompatMemory&) = delete;
    CompatMemory& operator=(const CompatMemory&) = delete;

    // Linux mmap: only MAP_PRIVATE|MAP_ANONYMOUS (optionally without
    // MAP_FIXED) is supported. fd must be -1, offset 0, length > 0.
    MapResult mmap(uintptr_t hint, size_t length, int prot, int flags, int fd, int64_t offset);

    // Linux munmap: releases whole mappings only.
    MapResult munmap(uintptr_t address, size_t length);

    // Linux mprotect: R/W honored; PROT_EXEC recorded but not granted.
    MapResult mprotect(uintptr_t address, size_t length, int prot);

    // Linux mremap: in-place shrink and MREMAP_MAYMOVE grow of a tracked
    // anonymous mapping. MREMAP_FIXED / MREMAP_DONTUNMAP are refused.
    MapResult mremap(uintptr_t address, size_t old_length, size_t new_length, int flags);

    // Linux madvise over tracked anonymous private memory. MADV_DONTNEED
    // zeroes the range; other accepted advice values are documented no-ops.
    MapResult madvise(uintptr_t address, size_t length, int advice);

    // Linux brk: brk(0) -> current break; brk(addr) -> new break or the
    // unchanged current break when the request cannot be honored.
    BrkResult brk(uintptr_t request);

    // True when [addr, addr+len) lies inside a live anonymous mapping.
    bool is_mapped(uintptr_t addr, size_t len) const;

    // Current program break (Linux brk(0) equivalent).
    uintptr_t current_break() const { return break_; }
    uintptr_t heap_start() const { return heap_start_; }

    size_t mapping_count() const { return mappings_.size(); }

  private:
    struct Mapping {
        uintptr_t addr;
        size_t size;
        int prot;
    };
    bool commit_pages(uintptr_t addr, size_t len);

    // Heap (brk) arena
    uintptr_t heap_start_ = 0;
    uintptr_t heap_end_ = 0; // exclusive, platform mapping limit
    uintptr_t break_ = 0;    // current program break
    void* heap_base_ = nullptr;

    // Anonymous mmap arena
    std::vector<Mapping> mappings_;

    bool brk_ready_ = false;
};

} // namespace compat
} // namespace ps5droid
