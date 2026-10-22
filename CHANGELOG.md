# Changelog

All notable changes to PS5Droid are documented here. The project follows
its honesty policy (docs/development.md, section 6): nothing in these
notes claims console execution that has not happened.

## [1.0.2] - 2026-10-22

### Fixed (correctness)

* **Linux `O_*` flag mapping in the guest fd table.** `open`/`openat`
  previously interpreted the *host platform's* flag values (e.g.
  `O_CREAT = 0x200` as on Darwin) instead of the Linux x86_64 UAPI
  values the guest ABI actually issues (`O_CREAT = 0x40`,
  `O_TRUNC = 0x200`, `O_APPEND = 0x400`). Linux constants now live in
  `linux_abi.h` (`kOCreat`, `kOExcl`, `kOTrunc`, `kOAppend`, ...) and
  are translated explicitly, with `O_EXCL` → `EEXIST`,
  missing-without-`O_CREAT` → `ENOENT`, and an append/truncate/access
  mode table documented in `fd_table.cpp`.
* **epoll semantics.** `EPOLL_CTL_ADD` of an already-registered fd now
  fails with `EEXIST` and `DEL`/`MOD` of an unregistered fd with
  `ENOENT`, matching Linux. `epoll_wait` is now readiness-based:
  entries are reported only when their target fd is currently ready
  (eventfd counters drive readiness; files/streams are always ready),
  with 0 / positive / negative timeout handling instead of returning
  the whole registration list unconditionally.
* **eventfd.** Implemented `EFD_SEMAPHORE` (reads return 1 and
  decrement), the kernel counter ceiling (`0xfffffffffffffffe`) with
  `EINVAL` on overflow writes, thread-safe counter access, and refused
  `initval` above the ceiling.
* **`pread64` no longer moves the shared file offset** (POSIX
  positioned-read semantics), and short reads from stdio errors map to
  `EIO` instead of being silently accepted.
* **Lowest-free fd allocation.** The guest fd table now reuses the
  lowest free descriptor number like Linux `alloc_fd` instead of a
  monotonically increasing counter.
* **`mmap` now requires `MAP_PRIVATE`** (Linux requires exactly one of
  `MAP_SHARED`/`MAP_PRIVATE`; the experiment models the private
  anonymous flavor only).
* **`openat`/`newfstatat` validate `dirfd`**: anything other than
  `AT_FDCWD` (-100) returns `EBADF` instead of being ignored.
* **Honesty fix (policy violation):** the APK inspector returned a
  fabricated package name (`"com.ps5droid.research.sample"`). Binary
  AXML parsing is not implemented, so `package_name` is now always
  empty and a new `package_note` field explains the limitation; a test
  asserts no placeholder is ever produced.

### Added (Linux ABI surface, +10 syscalls)

The emulated surface grew from 22 to 31 syscalls (79 tracked total):
`writev` (guest iovecs, `IOV_MAX` enforced), `stat`, `newfstatat`
(AT_FDCWD, `AT_SYMLINK_NOFOLLOW` via lstat), `access`
(F_OK/R_OK/W_OK/X_OK), `mkdir`, `unlink`, `getcwd`, `gettimeofday`,
`clock_getres`, and `getrandom` (PARTIAL: host `/dev/urandom`, pool
distinction not modeled, Orbis availability an explicit [ASSUMPTION]).
Operations propagate real Linux errnos (`EEXIST`, `ENOENT`, `EINVAL`,
`ESPIPE`, `ERANGE`, `EAGAIN`, ...) through the fd-table interface.

### Added (PS5 payload)

* All six launcher actions now have real on-console screens:
  probes, status (runtime phase + structured blockers), paged Linux ABI
  matrix (L1/R1 paging), and about. Previously three actions dumped to
  the About screen.
* New portable `ui/screens` module builds all screen content (used by
  both the host CLI and the payload) and is unit-tested on the host,
  keeping PS5-only code a thin render/input loop.
* `ps5droid.elf` status is unchanged: implemented, never executed on a
  console.

### Added (project)

* Version plumbing: `src/version.h`, CMake project version 1.0.2, and
  `ps5droid-host --version`.
* `CHANGELOG.md` (this file).
* New test files: `test_compat_fs_syscalls.cpp`, `test_screens.cpp`;
  20 new unit tests overall (45 → 65).

### Changed (docs / CI)

* README status table, `docs/architecture.md` (module map, matrix
  numbers, roadmap reality check) and `docs/platform-constraints.md`
  section 6 now cite the v1.0.2 matrix counts
  (79 = 31 emulated / 4 partial / 44 unsupported); the counts are pinned
  by `matrix_counts_match_v1_0_2_release` so docs and code cannot drift.
* Toolchain path documentation fixed: the ps5-payload-sdk release zip
  installs the toolchain at `<sdk>/toolchain/prospero.cmake` (source
  checkouts: `<sdk>/host/toolchain/`). README and development guide
  previously documented only the source-checkout path.
* CI PS5 configure step now accepts both SDK layouts.
* Sanitizer command in the development guide includes
  `src/android/binder/*.cpp` (previously missed).

## [0.1.0] - initial baseline

* Portable platform layer, Linux ABI dispatch experiment, capability
  probes + runtime manager with structured blockers, image manifest +
  SHA-256 + ext4 superblock probe, launcher UI + 8x8 font, guest fd
  table + epoll/eventfd (first cut), bionic ELF inspection, in-process
  Binder engine, APK structural probe, host CLI, PS5 payload build path,
  CI (lint + host matrix + PS5 cross-build).
