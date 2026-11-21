// PS5Droid - Bionic Loader & TLS/Auxv Feasibility Inspector.
//
// Milestone 4: Inspection of x86_64 ELF Bionic binaries (headers, PT_LOAD,
// PT_TLS, PT_INTERP, PT_DYNAMIC) and construction of Linux/Bionic initial
// stack and auxiliary vector (auxv) structures.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps5droid {
namespace compat {

struct Elf64PhdrInfo {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

struct BionicElfInfo {
    bool valid = false;
    uint64_t entry_point = 0;
    uint64_t phdr_addr = 0; // in-memory address of the program headers (from PT_LOAD)
    std::string interp;
    bool is_bionic = false;
    uint64_t tls_filesz = 0;
    uint64_t tls_memsz = 0;
    uint64_t tls_align = 0;
    std::vector<Elf64PhdrInfo> phdrs;
    std::string error;
};

struct AuxvEntry {
    uint64_t type;
    uint64_t val;
};

class BionicLoader {
  public:
    static BionicElfInfo parse_header(const void* data, size_t size);
    static std::vector<AuxvEntry> build_auxv(uint64_t entry, uint64_t phdr, uint64_t phnum,
                                             uint64_t pagesz = 4096);
    static std::vector<uint8_t> setup_initial_stack(const BionicElfInfo& info,
                                                    const std::vector<std::string>& args,
                                                    const std::vector<std::string>& env,
                                                    uint64_t stack_base = 0x7fffffffe000ull);
};

} // namespace compat
} // namespace ps5droid
