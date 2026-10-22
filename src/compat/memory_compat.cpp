// PS5Droid - Linux memory semantics emulation (implementation).
#include "compat/memory_compat.h"

#include <cstring>

#include "platform/platform.h"

namespace ps5droid {
namespace compat {

namespace {

constexpr size_t kHeapSize = 1024 * 1024; // 1 MiB brk arena

size_t page_align_up_sz(size_t v) {
    size_t ps = platform::page_size();
    return (v + ps - 1) & ~(ps - 1);
}

} // namespace

CompatMemory::CompatMemory() = default;

CompatMemory::~CompatMemory() {
    for (const Mapping& m : mappings_) {
        platform::unmap(reinterpret_cast<void*>(m.addr), m.size);
    }
    if (heap_base_ != nullptr) {
        platform::unmap(heap_base_, kHeapSize);
    }
}

MapResult CompatMemory::mmap(uintptr_t hint, size_t length, int prot, int flags, int fd,
                             int64_t offset) {
    if (length == 0) {
        return {nullptr, linux_abi::kEinval, "mmap length is zero"};
    }
    if (fd != -1) {
        return {nullptr, linux_abi::kEinval, "only anonymous mappings (fd == -1) are supported"};
    }
    if (offset != 0) {
        return {nullptr, linux_abi::kEinval, "file-backed mappings are not supported"};
    }
    // Linux requires exactly one of MAP_SHARED / MAP_PRIVATE. Only the
    // private anonymous flavor is modeled, so MAP_PRIVATE must be present.
    if ((flags & linux_abi::kMapAnonymous) == 0 || (flags & linux_abi::kMapPrivate) == 0 ||
        (flags & linux_abi::kMapShared) != 0) {
        return {nullptr, linux_abi::kEinval, "only MAP_PRIVATE|MAP_ANONYMOUS is supported"};
    }
    if ((flags & linux_abi::kMapFixed) != 0) {
        return {nullptr, linux_abi::kEinval, "MAP_FIXED is not supported in this experiment"};
    }
    (void)hint; // treated as a suggestion only; the allocator picks pages

    size_t aligned = page_align_up_sz(length);
    void* p = platform::map_anonymous(aligned);
    if (p == nullptr) {
        return {nullptr, linux_abi::kEnomem, "host map_anonymous failed"};
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    if (prot != (linux_abi::kProtRead | linux_abi::kProtWrite) && prot != linux_abi::kProtRead) {
        // Adjust to the requested protection (exec never granted).
        bool want_r = (prot & linux_abi::kProtRead) != 0;
        bool want_w = (prot & linux_abi::kProtWrite) != 0;
        if (!platform::protect_memory(p, aligned, want_r, want_w)) {
            platform::unmap(p, aligned);
            return {nullptr, linux_abi::kEnomem, "mprotect after mmap failed"};
        }
    }

    mappings_.push_back(Mapping{addr, aligned, prot});
    return {p, 0, "anonymous private mapping"};
}

MapResult CompatMemory::munmap(uintptr_t address, size_t length) {
    if (length == 0) {
        return {nullptr, linux_abi::kEinval, "munmap length is zero"};
    }
    for (size_t i = 0; i < mappings_.size(); ++i) {
        const Mapping& m = mappings_[i];
        if (address == m.addr && length == m.size) {
            if (!platform::unmap(reinterpret_cast<void*>(address), length)) {
                return {nullptr, linux_abi::kEinval, "host unmap failed"};
            }
            mappings_.erase(mappings_.begin() + static_cast<long>(i));
            return {reinterpret_cast<void*>(0), 0, "released"};
        }
    }
    // Interior or partial ranges are a documented limitation (kPartial).
    return {nullptr, linux_abi::kEinval,
            "only exact whole-mapping munmap is supported in this "
            "experiment"};
}

MapResult CompatMemory::mprotect(uintptr_t address, size_t length, int prot) {
    if (length == 0) {
        return {nullptr, linux_abi::kEinval, "mprotect length is zero"};
    }
    if ((prot & ~(linux_abi::kProtRead | linux_abi::kProtWrite | linux_abi::kProtExec)) != 0) {
        return {nullptr, linux_abi::kEinval, "unsupported prot bits"};
    }
    for (const Mapping& m : mappings_) {
        if (address >= m.addr && address + length <= m.addr + m.size) {
            bool want_r = (prot & linux_abi::kProtRead) != 0;
            bool want_w = (prot & linux_abi::kProtWrite) != 0;
            if (platform::protect_memory(reinterpret_cast<void*>(address), length, want_r,
                                         want_w)) {
                return {reinterpret_cast<void*>(0), 0,
                        (prot & linux_abi::kProtExec)
                            ? "R/W applied; PROT_EXEC not granted (no JIT)"
                            : "protection applied"};
            }
            return {nullptr, linux_abi::kEinval, "host mprotect failed"};
        }
    }
    return {nullptr, linux_abi::kEnomem, "range is not a tracked mapping"};
}

bool CompatMemory::commit_pages(uintptr_t /*addr*/, size_t /*len*/) {
    // The heap arena is mapped eagerly in brk(); nothing else to do.
    return true;
}

BrkResult CompatMemory::brk(uintptr_t request) {
    if (!brk_ready_) {
        void* p = platform::map_anonymous(kHeapSize);
        if (p == nullptr) {
            return {0, "heap arena allocation failed"};
        }
        heap_base_ = p;
        heap_start_ = reinterpret_cast<uintptr_t>(p);
        heap_end_ = heap_start_ + kHeapSize;
        break_ = heap_start_; // Linux: initial brk == start of heap region
        brk_ready_ = true;
    }

    if (request == 0 || request < heap_start_ || request > heap_end_) {
        // Linux returns the CURRENT break when the request fails.
        return {break_, request == 0 ? "query"
                                     : "request refused "
                                       "(outside arena)"};
    }
    if (request > break_) {
        // Pages come from a pre-mapped arena; they are already committed.
        (void)commit_pages(break_, request - break_);
    }
    break_ = request;
    return {break_, "break updated"};
}

bool CompatMemory::is_mapped(uintptr_t addr, size_t len) const {
    for (const Mapping& m : mappings_) {
        if (addr >= m.addr && addr + len <= m.addr + m.size)
            return true;
    }
    if (addr >= heap_start_ && addr + len <= heap_end_)
        return true;
    return false;
}

} // namespace compat
} // namespace ps5droid
