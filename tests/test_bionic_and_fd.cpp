// PS5Droid - Milestone 3 & 4 unit tests.
#include "test_framework.h"

#include <cstring>

#include "compat/bionic_loader.h"
#include "compat/fd_table.h"
#include "platform/platform.h"

using namespace ps5droid::compat;

PS5DROID_TEST(fd_table_basic_rw_seek) {
    CompatFdTable table;
    std::string path = "test_fd_table.tmp";
    // Linux x86_64 O_CREAT|O_TRUNC|O_RDWR values (guest ABI, not host).
    int fd = table.open_file(path, 0x40 | 0x200 | 0x2, 0644);
    CHECK_MSG(fd >= 3, "open_file failed");

    const char* msg = "hello fd_table";
    int64_t w = table.write(fd, msg, strlen(msg));
    CHECK_MSG(w == static_cast<int64_t>(strlen(msg)), "write failed");

    int64_t pos = table.lseek(fd, 0, 0);
    CHECK_MSG(pos == 0, "lseek failed");

    char buf[32] = {0};
    int64_t r = table.read(fd, buf, sizeof(buf));
    CHECK_MSG(r == static_cast<int64_t>(strlen(msg)), "read failed");
    CHECK_MSG(strcmp(buf, msg) == 0, "data mismatch");

    StatBuf st;
    CHECK_MSG(table.fstat(fd, &st), "fstat failed");
    CHECK_MSG(st.st_size == static_cast<int64_t>(strlen(msg)), "stat size mismatch");

    int dup_fd = table.dup(fd);
    CHECK_MSG(dup_fd >= 0 && dup_fd != fd, "dup failed");

    // Both descriptors now reference the same entry: a write through the
    // duplicate must be visible through the original.
    const char* extra = "!";
    CHECK_MSG(table.write(dup_fd, extra, 1) == 1, "dup write failed");
    CHECK_MSG(table.lseek(fd, 0, 2 /*SEEK_END*/) == static_cast<int64_t>(strlen(msg) + 1),
              "shared file entry after dup");

    table.close(fd);
    table.close(dup_fd);
    ps5droid::platform::remove_file(path);
}

PS5DROID_TEST(fd_table_lowest_free_allocation) {
    CompatFdTable table;
    std::string a = "test_fd_alloc_a.tmp";
    std::string b = "test_fd_alloc_b.tmp";
    int flags = 0x40 | 0x200 | 0x2; // O_CREAT|O_TRUNC|O_RDWR (Linux)
    int fd1 = table.open_file(a, flags, 0644);
    int fd2 = table.open_file(b, flags, 0644);
    CHECK_MSG(fd1 >= 3 && fd2 == fd1 + 1, "sequential allocation");

    // Linux allocates the lowest free number: closing fd1 makes the next
    // open return fd1 again, not a fresh high-water mark.
    CHECK_MSG(table.close(fd1) == 0, "close failed");
    std::string c = "test_fd_alloc_c.tmp";
    int fd3 = table.open_file(c, flags, 0644);
    CHECK_MSG(fd3 == fd1, "lowest free fd must be reused");

    table.close(fd2);
    table.close(fd3);
    ps5droid::platform::remove_file(a);
    ps5droid::platform::remove_file(b);
    ps5droid::platform::remove_file(c);
}

PS5DROID_TEST(fd_table_fcntl_and_flags) {
    using namespace ps5droid::linux_abi;
    CompatFdTable table;
    std::string path = "test_fcntl.tmp";
    int fd = table.open_file(path, kOCreat | kOTrunc | kORdwr, 0644);
    CHECK_MSG(fd >= 3, "open_file failed");

    // Descriptor flags (FD_CLOEXEC)
    CHECK_MSG(table.fcntl(fd, kFGetfd, 0) == 0, "FD_CLOEXEC initially 0");
    CHECK_MSG(table.fcntl(fd, kFSetfd, kFdCloexec) == 0, "F_SETFD failed");
    CHECK_MSG(table.fcntl(fd, kFGetfd, 0) == kFdCloexec, "FD_CLOEXEC set");

    // F_DUPFD (minfd)
    int dup_fd = table.fcntl(fd, kFDupfd, 10);
    CHECK_MSG(dup_fd >= 10, "F_DUPFD minfd requirement");
    CHECK_MSG(table.fcntl(dup_fd, kFGetfd, 0) == 0, "dup clears FD_CLOEXEC");

    // F_DUPFD_CLOEXEC
    int dup_cloexec = table.fcntl(fd, kFDupfdCloexec, 20);
    CHECK_MSG(dup_cloexec >= 20, "F_DUPFD_CLOEXEC minfd requirement");
    CHECK_MSG(table.fcntl(dup_cloexec, kFGetfd, 0) == kFdCloexec,
              "F_DUPFD_CLOEXEC sets FD_CLOEXEC");

    // Status flags (O_APPEND / O_NONBLOCK)
    int fl = table.fcntl(fd, kFGetfl, 0);
    CHECK_MSG((fl & kOAccmode) == kORdwr, "O_RDWR status flag");
    CHECK_MSG(table.fcntl(fd, kFSetfl, fl | kONonblock | kOAppend) == 0, "F_SETFL failed");
    int new_fl = table.fcntl(fd, kFGetfl, 0);
    CHECK_MSG((new_fl & kONonblock) != 0, "O_NONBLOCK set");
    CHECK_MSG((new_fl & kOAppend) != 0, "O_APPEND set");

    // Modifying status flags on the open file description reflects on dups
    CHECK_MSG((table.fcntl(dup_fd, kFGetfl, 0) & kONonblock) != 0,
              "status flags shared across dups");

    table.close(fd);
    table.close(dup_fd);
    table.close(dup_cloexec);
    ps5droid::platform::remove_file(path);
}

PS5DROID_TEST(bionic_loader_hdr_and_stack) {
    // Construct fake ELF64 header with one PT_LOAD covering the headers.
    uint8_t elf_bytes[128] = {0};
    elf_bytes[0] = 0x7f;
    elf_bytes[1] = 'E';
    elf_bytes[2] = 'L';
    elf_bytes[3] = 'F';
    elf_bytes[4] = 2;   // 64-bit
    elf_bytes[18] = 62; // x86_64

    // e_phoff = 64, e_phentsize = 56, e_phnum = 1
    uint64_t phoff = 64;
    memcpy(elf_bytes + 32, &phoff, sizeof(phoff));
    uint16_t phentsize = 56;
    memcpy(elf_bytes + 54, &phentsize, sizeof(phentsize));
    uint16_t phnum = 1;
    memcpy(elf_bytes + 56, &phnum, sizeof(phnum));

    // RawElf64Phdr at offset 64: PT_LOAD, offset 0, vaddr 0x400000, filesz 200
    uint8_t* ph = elf_bytes + 64;
    uint32_t p_type = 1;
    uint32_t p_flags = 5;
    uint64_t p_offset = 0;
    uint64_t p_vaddr = 0x400000;
    uint64_t p_filesz = 200;
    memcpy(ph + 0, &p_type, sizeof(p_type));
    memcpy(ph + 4, &p_flags, sizeof(p_flags));
    memcpy(ph + 8, &p_offset, sizeof(p_offset));
    memcpy(ph + 16, &p_vaddr, sizeof(p_vaddr));
    memcpy(ph + 32, &p_filesz, sizeof(p_filesz));

    BionicElfInfo info = BionicLoader::parse_header(elf_bytes, sizeof(elf_bytes));
    CHECK_MSG(info.valid, "ELF header parse failed");
    CHECK_MSG(info.phdr_addr == 0x400000 + 64, "AT_PHDR address must come from PT_LOAD");

    const uint64_t base = 0x1000;
    auto stack = BionicLoader::setup_initial_stack(info, {"app", "--flag"}, {"ENV=1"}, base);
    CHECK_MSG(!stack.empty(), "stack setup failed");

    const uint64_t* words = reinterpret_cast<const uint64_t*>(stack.data());
    CHECK_MSG(words[0] == 2, "argc must be 2");

    auto string_at = [&](uint64_t pointer) -> const char* {
        if (pointer < base || pointer - base >= stack.size())
            return nullptr;
        return reinterpret_cast<const char*>(stack.data() + (pointer - base));
    };
    CHECK_MSG(string_at(words[1]) != nullptr && strcmp(string_at(words[1]), "app") == 0,
              "argv[0] must point at real bytes");
    CHECK_MSG(string_at(words[2]) != nullptr && strcmp(string_at(words[2]), "--flag") == 0,
              "argv[1] must point at real bytes");
    CHECK_MSG(words[3] == 0, "argv must be NULL-terminated");
    CHECK_MSG(string_at(words[4]) != nullptr && strcmp(string_at(words[4]), "ENV=1") == 0,
              "envp[0] must point at real bytes");
    CHECK_MSG(words[5] == 0, "envp must be NULL-terminated");

    // auxv starts after envp; AT_PHDR must carry the derived address.
    CHECK_MSG(words[6] == 3, "AT_PHDR must be the first auxv entry");
    CHECK_MSG(words[7] == 0x400000 + 64, "AT_PHDR value must match phdr_addr");
}
