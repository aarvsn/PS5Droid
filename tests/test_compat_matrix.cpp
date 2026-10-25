// PS5Droid - Linux ABI table + FreeBSD mapping consistency tests.
#include "test_framework.h"

#include <cstring>
#include <set>

#include "compat/linux_abi.h"

using namespace ps5droid::linux_abi;

PS5DROID_TEST(table_entries_valid) {
    std::set<int> seen;
    for (int i = 0; i < kSyscallTableSize; ++i) {
        const SyscallInfo& e = kSyscallTable[i];
        CHECK_MSG(e.name != nullptr, "entry without name");
        CHECK_MSG(e.note != nullptr && e.note[0] != '\0', "entry without note");
        CHECK_MSG(seen.insert(e.number).second, "duplicate syscall number");
    }
    CHECK_MSG(kSyscallTableSize >= 70, "table suspiciously small");
}

PS5DROID_TEST(matrix_counts_match_v1_0_3_release) {
    int emulated = 0, partial = 0, unsupported = 0;
    for (int i = 0; i < kSyscallTableSize; ++i) {
        switch (kSyscallTable[i].support) {
        case kEmulated:
            ++emulated;
            break;
        case kPartial:
            ++partial;
            break;
        case kUnsupported:
            ++unsupported;
            break;
        }
    }
    CHECK_MSG(emulated == 36, "36 emulated syscalls in v1.0.3");
    CHECK_MSG(partial == 4, "4 partial syscalls in v1.0.3");
    CHECK_MSG(unsupported == 39, "39 unsupported syscalls in v1.0.3");
    CHECK_MSG(kSyscallTableSize == 79, "79 tracked syscalls in v1.0.3");
}

PS5DROID_TEST(names_resolve) {
    CHECK_MSG(strcmp(syscall_name(kMmap), "mmap") == 0, "mmap name");
    CHECK_MSG(strcmp(syscall_name(kFutex), "futex") == 0, "futex name");
    CHECK_MSG(strcmp(syscall_name(kGetdents64), "getdents64") == 0, "getdents64 name");
    CHECK_MSG(strcmp(syscall_name(kWritev), "writev") == 0, "writev must be tracked");
    CHECK_MSG(strcmp(syscall_name(kStat), "stat") == 0, "stat must be tracked");
    CHECK_MSG(strcmp(syscall_name(kGetrandom), "getrandom") == 0, "getrandom tracked");
    CHECK_MSG(strcmp(syscall_name(kPipe2), "pipe2") == 0, "pipe2 tracked");
    CHECK_MSG(strcmp(syscall_name(kUname), "uname") == 0, "uname tracked");
    CHECK_MSG(strcmp(syscall_name(kPrctl), "prctl") == 0, "prctl tracked");
    CHECK_MSG(syscall_name(9999) == nullptr, "unknown must be null");
    CHECK_MSG(syscall_name(kOpen) != nullptr, "open must be tracked");
}

PS5DROID_TEST(freebsd_numbers_match_sdk_sources) {
    struct Expected {
        int linux_nr;
        int freebsd_nr;
    };
    const Expected expected[] = {
        {kRead, 3},    {kWrite, 4},   {kOpen, 5},      {kClose, 6},  {kGetpid, 20},
        {kAccess, 33}, {kMunmap, 73}, {kMprotect, 74}, {kMmap, 477}, {kLseek, 478},
    };
    for (const auto& e : expected) {
        const FreebsdMapping* m = freebsd_mapping(e.linux_nr);
        CHECK_MSG(m != nullptr, "mapping missing");
        CHECK_MSG(m->freebsd_nr == e.freebsd_nr, "wrong FreeBSD number");
    }
    CHECK_MSG(freebsd_mapping(kFutex) == nullptr, "futex unverified");
    CHECK_MSG(freebsd_mapping(kClone) == nullptr, "clone unverified");
    CHECK_MSG(freebsd_mapping(kExecve) == nullptr, "execve unverified");
    CHECK_MSG(freebsd_mapping(9999) == nullptr, "unknown unmapped");
}

PS5DROID_TEST(linux_numbers_are_x86_64_facts) {
    CHECK(kRead == 0);
    CHECK(kWrite == 1);
    CHECK(kMmap == 9);
    CHECK(kMprotect == 10);
    CHECK(kMunmap == 11);
    CHECK(kBrk == 12);
    CHECK(kClone == 56);
    CHECK(kExecve == 59);
    CHECK(kFutex == 202);
    CHECK(kGettid == 186);
    CHECK(kClockGettime == 228);
    CHECK(kExitGroup == 231);
    CHECK(kOpenat == 257);
    CHECK(kRead != freebsd_mapping(kRead)->freebsd_nr);
    CHECK(kMmap != freebsd_mapping(kMmap)->freebsd_nr);
}
