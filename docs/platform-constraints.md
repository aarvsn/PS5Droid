# PS5 Platform Constraints

Status: research document. Every claim below is labeled:

* **[VERIFIED-SOURCE]** — read directly from primary sources available to
  this project (ps5-payload-sdk / elfldr / SDL-port source trees, cloned
  2026-10-18; file paths cited).
* **[VERIFIED-TEST]** — exercised by a test in this repository on the host
  backend (see `tests/`); on-console equivalence still requires a console
  run.
* **[ASSUMPTION]** — plausible, unverified; must be probed before relying
  on it. These feed Milestone 2 probe work.

Target: a jailbroken PlayStation 5 running native homebrew payloads, as
enabled by the open-source tooling described below. PS5Droid does not use,
download or redistribute any proprietary Sony SDK.

## 1. CPU and kernel

* **[VERIFIED-SOURCE]** The PS5 runs Orbis OS on an x86_64 AMD CPU. The
  ps5-payload-sdk CMake toolchain (`sdk/host/toolchain/prospero.cmake`)
  sets `CMAKE_SYSTEM_PROCESSOR x86_64`, `CMAKE_SYSTEM_NAME FreeBSD`,
  `CMAKE_SYSTEM_VERSION 9` — i.e. a FreeBSD 9-lineage kernel ("prospero"
  is the PS5's internal codename).
* **[VERIFIED-SOURCE]** The SDK requires host clang-18/lld-18 and produces
  standard ELF x86_64 executables; position-independent executables only
  (`CMAKE_POSITION_INDEPENDENT_CODE ON`, shared libs off by default).
* **[ASSUMPTION]** 8-core Zen 2 CPU; exact exposed core count for payloads
  should be probed at runtime (planned probe).

## 2. Executable format, loading and packaging

* **[VERIFIED-SOURCE]** Homebrew payloads are plain ELF binaries pushed
  over TCP. `ps5-payload-elfldr` (`elfldr/README.md`) accepts ELFs on
  port 9021, bootstraps itself via the ptrace syscall into the
  `SceRedisServer` process of a console already jailbroken by an ELF
  loader exploit (BD-J payload loader or a webkit-based kernel exploit),
  and spawns each payload as its own process.
* **[VERIFIED-SOURCE]** Firmware entry points are declared by hand in
  payloads; the SDK ships symbol stubs (`sdk/sce_stubs/*.c`) generated
  from decrypted modules, and the `install_app` sample demonstrates
  fake-signed SELF creation (`make_fself.py`) plus app installation via
  `SceAppInstUtil` with a FAKE title id. This is the ecosystem path for
  installed apps; PS5Droid currently targets the simpler ELF-payload path
  and produces **no PKG**.
* **[VERIFIED-SOURCE]** The SDK's `crt/` provides `_start`, a dynamic
  loader for payload-local shared objects (`rtld*.c`), and an
  `__crt_syscall` entry for direct syscalls (`crt/syscall.h`).
* **[ASSUMPTION]** which exploit/loader pairs support which firmware
  versions changes over time; verify against the loader projects when
  planning console testing.

## 3. Syscall ABI — the central constraint

**[VERIFIED-SOURCE]** `sdk/crt/syscall.h` lists the Orbis syscall numbers
the SDK relies on; they are FreeBSD-amd64 numbers, NOT Linux numbers:

| Operation | Linux x86_64 | Orbis (FreeBSD) | Note |
|---|---|---|---|
| read | 0 | 3 | verified both sides |
| write | 1 | 4 | verified both sides |
| open | 2 | 5 | verified both sides |
| close | 3 | 6 | verified both sides |
| getpid | 39 | 20 | verified both sides |
| access | 21 | 33 | verified (sdk) |
| munmap | 11 | 73 | verified both sides |
| mprotect | 10 | 74 | verified both sides |
| stat | 4 | 188 | verified (sdk) |
| mmap | 9 | 477 | verified both sides |
| lseek | 8 | 478 | verified (sdk) |
| sysctl | — | 202 | Orbis-only entry in SDK |
| mdbg_call / sprx_dlsym / sprx_load / sprx_unload | — | 573/591/594/595 | Orbis payload-loader services |

Consequences (all encoded in `src/compat/linux_abi.h` and tested in
`tests/test_compat_matrix.cpp`):

1. Linux x86_64 binaries cannot run directly: raw `syscall` instructions
   carry different numbers and different argument conventions.
2. A compat layer must translate numbers, validate flags (Linux
   `MAP_ANONYMOUS`/`MAP_PRIVATE` constants), and emulate missing
   functionality.
3. Only the table above is *verified* for Orbis. PS5Droid's translation
   table deliberately maps nothing else; `freebsd_mapping()` returns null
   for unverified numbers (enforced by test).

## 4. Memory

* **[VERIFIED-SOURCE]** mmap/munmap/mprotect exist in the SDK libc
  (`sdk/libc/mman.c` implements mmap over syscall 477 with standard
  POSIX semantics; the raw `syscall` instruction is used from userland).
* **[VERIFIED-SOURCE]** GPU-visible "direct memory" is allocated through
  dedicated calls. The SDL port demonstrates the full flow:
  `sceKernelAllocateMainDirectMemory(size, 0x20000, 3, &phys)` then
  `sceKernelMapDirectMemory(&vaddr, size, 0x33, 0, phys, 0x20000)`
  (`sdl-ps5/src/video/ps5/SDL_ps5video.c`), with
  `sceKernelReleaseDirectMemory` to free.
* **[VERIFIED-TEST]** `platform::map_anonymous`/`unmap`/
  `protect_memory` work on the host backend (zero-filled pages, R/W
  protection transitions). The same code paths compile for the PS5
  backend; console verification is pending.
* **[ASSUMPTION]** `sysconf(_SC_PAGESIZE)` works on Orbis; the platform
  layer falls back to 4096 and the `page_size` probe reports reality.
* **[ASSUMPTION]** PROT_EXEC allocations for JIT (ART requires one) are a
  dedicated research topic; current compat code never requests exec pages
  and documents that restriction (`memory_compat.cpp`).

## 5. Threads, synchronization, signals

* **[VERIFIED-SOURCE]** The SDK libc ships C11 threads
  (`sdk/libc/thrd.c`, `mtx.c`, `cnd.c`, `tss.c`). PS5Droid's portable
  Thread/Mutex/CondVar build on `<threads.h>` for both backends.
* **[VERIFIED-SOURCE]** FreeBSD umtx infrastructure headers exist in the
  SDK (`include/freebsd/sys/umtx.h`, `thr.h`); a `_umtx_op`-backed futex
  is a candidate optimization, not yet wired (the syscall number is NOT
  in the verified set).
* **[VERIFIED-TEST]** Threads, mutex/condvar, and a futex-like
  wait/wake registry work on host (`tests/test_platform_threads.cpp`).
* **[ASSUMPTION/DESIGN]** Linux signal semantics (`rt_sigaction` etc.)
  are NOT implementable on Orbis in the near term; bionic's signal-heavy
  paths are a known risk. The compat layer returns `-ENOSYS` for the
  signal family.

## 6. Linux ABI compatibility matrix (v1.0.3)

Authoritative live output: `./build/ps5droid-host --matrix`. Summary of
the 79 tracked syscalls:

* **EMULATED (36)** — real in-process behavior, unit tested:
  memory (`mmap`, `brk`), clocks (`clock_gettime`, `clock_getres`,
  `gettimeofday`, `nanosleep`, `sched_yield`), identity
  (`getpid`, `gettid`), system & process (`uname`, `prctl`), the guest fd table family (`read`, `write`,
  `writev`, `open`, `openat`, `close`, `lseek`, `pread64`, `fstat`,
  `dup`, `dup2`, `dup3`, `pipe2`, `epoll_create1`, `epoll_ctl`, `epoll_wait`,
  `eventfd2`), the path family (`stat`, `newfstatat`, `access`, `readlink`,
  `mkdir`, `unlink`, `getcwd`), and `exit_group`
* **PARTIAL (4)** — implemented with documented restrictions:
  `munmap` (whole mappings only), `mprotect` (R/W only; PROT_EXEC not
  granted), `futex` (WAIT/WAKE + PRIVATE flag only), `getrandom`
  (host `/dev/urandom`; pool distinction not modeled)
* **UNSUPPORTED (39)** — return `-ENOSYS` with an explanatory note. The
  hard blockers remain `clone/fork/vfork/execve` (no process creation
  for payloads), the socket and signal families, and directory streams.

`getpid` returns the emulation-context id and `gettid` the platform
thread id — single-process model, documented in
`src/compat/syscall_dispatch.h`. The counts are pinned by
`tests/test_compat_matrix.cpp` (`matrix_counts_match_v1_0_3_release`)
so docs and code cannot drift silently.

## 7. Filesystem

* **[VERIFIED-SOURCE]** Full BSD VFS syscalls exist (SDK libc implements
  open/stat/opendir via wrappers; `list_files`, `mntinfo` samples). The
  SDK even bundles a **UFS library** (`sdk/libufs/`) matching the
  console's internal disk format.
* **[VERIFIED-TEST]** read/write/exists/remove/mkdir/list_dir work on
  host; identical stdio/dirent code compiles for PS5.
* **[ASSUMPTION]** writable paths for payloads (e.g. `/data`,
  `/user/homebrew` per the toolchain's `CMAKE_INSTALL_PREFIX`) need a
  console probe before image storage design.
* **Constraint**: no loop devices, no FUSE, no ext4 driver → Android
  ext4 images can be *read* (superblock probe implemented in
  `image_manager.cpp`) but not mounted.

## 8. IPC and networking

* **[VERIFIED-SOURCE]** Event queues are FreeBSD kqueue objects:
  `sceKernelCreateEqueue` returns `struct kevent*` and
  `sceKernelWaitEqueue` waits on it (SDL port usage; `sys/event.h` in SDK
  headers). This is the natural epoll replacement for any event loop.
* **[VERIFIED-SOURCE]** BSD sockets + `libSceNet`/`libSceNetCtl` stubs
  exist; `http2_get` sample performs real networking.
* **[VERIFIED-TEST]** SPSC ring + block allocator (Binder-style
  transaction buffers) work cross-thread on host
  (`tests/test_ipc_ring.cpp`).
* **Constraint**: no binder/binderfs, no shared `/dev` IPC devices; any
  Android IPC must be re-hosted in-process.

## 9. Graphics, input, audio

* **[VERIFIED-SOURCE]** Video-out flow (all used by the SDL port):
  `sceVideoOutOpen(0xff,0,0,NULL)` → allocate/map direct memory →
  `sceVideoOutSetBufferAttribute2` with pixel format
  `0x8000000022000000` (A8R8G8B8 sRGB) → `sceVideoOutRegisterBuffers2` →
  flip events on an equeue → `sceVideoOutSubmitFlip` (vsync) →
  `sceKernelWaitEqueue`. PS5Droid's `ps5/display_ps5.cpp` implements
  exactly this flow; status: **implemented, never executed on console**.
* **[VERIFIED-SOURCE]** Pad: `scePadInit/Open/ReadState/Close` with the
  DualSense button bitmask and `ScePadData` layout as used by the SDL
  port (`sdl-ps5/src/joystick/ps5/SDL_ps5joystick.{h,c}`); PS5Droid's
  `ps5/input_ps5.cpp` mirrors it. Same status caveat.
* **[VERIFIED-SOURCE]** Proprietary graphics APIs exist as stubs
  (`libSceGnmDriver`, `libSceAgc`, `libSceGnmDriverForNeoMode`), and
  Khronos EGL/GLES2 headers are bundled in the SDK; an SDL2 port (zlib)
  exists. **No Vulkan driver is available** to homebrew; GPU-accelerated
  Android rendering is out of reach for now.
* **[VERIFIED-SOURCE]** `libSceAudioOut` stub symbols exist; PS5Droid has
  no audio design yet and reports audio as explicitly unsupported
  (`platform/audio.h`).
* **[VERIFIED-SOURCE]** `sceSystemServiceHideSplashScreen` and
  `sceKernelSendNotificationRequest` are used by SDK samples.

## 10. Firmware, jailbreak and privilege model

* **[VERIFIED-SOURCE]** Payloads require a console already running an
  exploit-based ELF loader (see `elfldr/README.md`); the loader
  ptrace-bootstraps into `SceRedisServer` and survives rest mode.
* **[VERIFIED-SOURCE]** The SDK exposes exploit-provided kernel read/write
  helpers (`include/ps5/kernel.h`: `kernel_copyin/copyout`, proc/ucred
  offsets) — evidence that *some* consoles grant kernel access, but
  PS5Droid deliberately depends only on plain userland facilities.
* **[ASSUMPTION]** Capability ceilings (which syscalls/privileges a
  payload process retains, e.g. for raw socket or process inspection)
  must be probed on console; `sdk/samples/test_privileges` exists as a
  reference tool.

## 11. Implications for running Android (Milestones 4-6 outlook)

1. bionic binaries: ELF x86_64 is the *right format* on paper, but bionic
   executables link against Linux-specific ELF/TLS/auxv layouts and issue
   Linux syscalls directly — both blocked without the (incomplete) compat
   layer and a loader PS5Droid does not have yet.
2. ART: needs JIT (exec pages), futex, robust threads, and a full fd
   table — all open items; see `docs/architecture.md` for the plan.
3. Binder, servicemanager, SurfaceFlinger: absent kernel driver and
   compositor; in-process re-hosting is the only route
   (`src/android/binder/binder_protocol.h`).

None of this means "impossible" — it means each item is a concrete,
testable work item, and nothing in this repository claims Android
applications run today.
