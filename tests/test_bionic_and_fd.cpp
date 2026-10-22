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

PS5DROID_TEST(bionic_loader_hdr_and_stack) {
    // Construct fake ELF64 header
    uint8_t elf_bytes[128] = {0};
    elf_bytes[0] = 0x7f;
    elf_bytes[1] = 'E';
    elf_bytes[2] = 'L';
    elf_bytes[3] = 'F';
    elf_bytes[4] = 2;   // 64-bit
    elf_bytes[18] = 62; // x86_64

    BionicElfInfo info = BionicLoader::parse_header(elf_bytes, sizeof(elf_bytes));
    CHECK_MSG(info.valid, "ELF header parse failed");

    auto stack = BionicLoader::setup_initial_stack(info, {"app"}, {"ENV=1"});
    CHECK_MSG(!stack.empty(), "stack setup failed");
}
