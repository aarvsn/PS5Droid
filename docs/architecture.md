# PS5Droid Architecture

Status: living document describing the implemented system and the planned
path toward native Android execution. Companion documents:
`platform-constraints.md` (what the PS5 allows), `waydroid-analysis.md`
(what upstream does), `development.md` (how to build/test).

## 1. Design goal and non-goals

**Goal**: a *native* Orbis homebrew payload that can eventually host
Android application components directly — no Linux boot, no VM, no
streaming, no conventional Waydroid install.

**Non-goals (explicitly)**: running a Linux kernel on the PS5, running
unmodified Android system images, Google Play / proprietary components.

## 2. Module map (implemented)

```
src/
├── version.h                 release version (single source of truth)
├── platform/                 portable platform layer
│   ├── platform.h            logging/time/memory/threads/fs API
│   ├── log.cpp               logging + backend identity
│   ├── memory.cpp            mmap-backed page allocator (shared by both ABIs)
│   ├── threads.cpp           C11-threads core + futex-like registry
│   ├── filesystem.cpp        stdio/dirent file access
│   ├── display.h/.cpp        Framebuffer + DisplayBackend + host impl
│   ├── audio.h/.cpp          AudioBackend (explicitly unsupported today)
│   └── ps5/                  PS5-only code (compiled solely in SDK builds)
│       ├── sce_decls.h       hand-maintained firmware declarations (provenance documented)
│       ├── display_ps5.cpp   video-out + direct-memory + flip queue
│       └── input_ps5.cpp     scePad backend
├── compat/                   Linux ABI experiment (Milestone 3+)
│   ├── linux_abi.h/.cpp      syscall constants + tables + verified FreeBSD mapping
│   ├── memory_compat.cpp     Linux brk + anonymous MAP_PRIVATE mmap semantics
│   ├── fd_table.h/.cpp       guest fd table: Linux O_* flags, lowest-free fds, guest pipes,
│   │                         readiness-based epoll, eventfd (EFD_SEMAPHORE/overflow)
│   ├── syscall_dispatch.cpp  Linux syscall number -> emulated op dispatch
│   ├── bionic_loader.cpp     x86_64 ELF inspection + auxv/initial-stack builder
│   └── ipc_compat.cpp        SPSC ring + Binder-style block allocator
├── android/
│   ├── sha256.cpp            FIPS 180-4 (clean-room)
│   ├── image_manager.cpp     manifest parsing + ext4 superblock + AXML parser + ZIP + build.prop
│   ├── runtime_manager.cpp   capability probes + honest start attempts
│   └── binder/
│       ├── binder_engine.h/.cpp  in-process Binder RPC engine (Milestone 5)
│       └── binder_protocol.h     research notes on the real kernel protocol
├── ui/
│   ├── launcher.cpp          menu state machine (portable)
│   ├── bitmap_font.cpp       8x8 font authored in-repo + renderer
│   └── screens.cpp           screen content builders shared by both entries
└── app/
    ├── main.cpp              host CLI: probes + ABI demo + matrix + version
    └── ps5_main.cpp          PS5 payload entry (SDK builds only)
```

Build rules enforced by CMake:

* `ps5droid_core` is portable; PS5-only sources are added only when the
  ps5-payload-sdk toolchain is active (`PROSPERO`).
* The host binary (`ps5droid-host`) and tests are never built for PS5,
  and the PS5 ELF is never built without the SDK toolchain.

## 3. Layering and data flow

```
+--------------------------------------------------------------+
| app/main.cpp (host)          app/ps5_main.cpp (PS5 payload)  |
+--------------------------------------------------------------+
| ui/launcher (menu model)  |  ui/bitmap_font (rendering)      |
+---------------------------+----------------------------------+
| android/runtime_manager   |  android/image_manager           |
|  - capability probes      |   - manifest + ext4 probe        |
|  - blocker reporting      |   - AXML + ZIP + build.prop      |
|                           |   - sha256 verification          |
+---------------------------+----------------------------------+
| compat/syscall_dispatch (Linux ABI experiment)                |
|   uses compat/memory_compat + platform::Futex                 |
+---------------------------------------------------------------+
| platform/ (memory, threads, fs, display, input)               |
|   posix backend (hosts/CI)   |   ps5 backend (Orbis payload)  |
+---------------------------------------------------------------+
```

The **single source tree** compiles for both backends; divergence points
are small and explicit (sce_decls.h, the two ps5 backend files, and
per-file `#ifdef PS5DROID_PLATFORM_PS5` sections in the platform core).

## 4. The Linux ABI experiment (Milestone 3, scope and honesty)

`SyscallDispatch::invoke(linux_nr, args)` accepts Linux x86_64 syscall
numbers and executes a curated subset:

* anonymous `mmap`/`munmap`/`mprotect` and `brk` over
  `compat::CompatMemory`, which in turn uses `platform::map_anonymous`
  (real OS mappings on both backends);
* `clock_gettime`, `clock_getres`, `gettimeofday`, `nanosleep`,
  `sched_yield` over platform clocks;
* `getpid`/`gettid` under the documented single-process model;
* `futex` WAIT/WAKE (private) over `platform::Futex` (registry-based,
  wake-atomicity mirrors Linux's kernel-side check-and-enqueue);
* a **guest fd table** (`compat::CompatFdTable`) with Linux `O_*` flag
  values, descriptor and status flag management via `fcntl`, lowest-free fd allocation, `dup`/`dup2`/`dup3`, in-process guest pipes (`pipe2`),
  positioned `pread64` that never moves the shared offset, eventfd counters
  honoring `EFD_SEMAPHORE` and the overflow ceiling, and `writev` gather output;
* **readiness-based epoll**: `epoll_ctl` enforces Linux `EEXIST`/
  `ENOENT` semantics and `epoll_wait` reports only entries whose target
  fd is currently ready (eventfd counters and pipes drive readiness);
* the path family `stat`/`newfstatat`/`unlinkat` (AT_FDCWD only), `access`,
  `mkdir`, `unlink`, `readlink`, `getcwd` over host stat/stdio/symlink calls;
* `uname` and `prctl` (process/thread naming);
* user and group identity (`getuid`, `getgid`, `geteuid`, `getegid` returning single-process guest identity);
* `getrandom` (PARTIAL) from the host `/dev/urandom`;
* `write` to the process streams (fd 1/2);
* `exit_group` flags the context.

Everything else returns `-ENOSYS` with a *stable, explanatory note*
sourced from the syscall table. The matrix (42 emulated / 4 partial /
34 unsupported = 80 tracked at the v1.0.4 release) is data, not prose:
`ps5droid-host --matrix` prints it, tests pin the counts, and the
release documents repeat them.

**Scope limitation (important)**: pointer arguments are host pointers.
This experiment demonstrates *ABI translation*, not address-space
isolation or multi-process hosting. Lifting that limitation is future
work and gated on process-model research.

## 5. Runtime manager and honesty model

`android::RuntimeManager` is the only component allowed to describe
overall system status:

* `probe()` runs eight real, in-process capability probes (page size,
  map/unmap, protect, threads, futex, filesystem, IPC ring, ABI mmap)
  and reports each result. Every "[OK]" corresponds to executed code.
* `attempt_start()` performs an Android-environment start attempt. It
  **always** ends in `kBlocked` today, with structured blockers:
  `process_model`, `binder`, `filesystem_layout`, `graphics_services`,
  `bionic_runtime`. The state machine cannot reach `kRunning` — asserted
  by tests (`phase_is_never_running`).

## 6. PS5 payload path

`ps5_main.cpp` hides the splash screen, initializes 1920x1080
double-buffered video-out (direct memory, vsync flips via event queue)
and the pad, then drives the launcher: every menu action has a real
screen (probes, status with structured blockers, paged Linux ABI matrix
via L1/R1, about), with all content built by the portable `ui/screens`
module and pinned by host tests. It exits cleanly releasing direct
memory. The firmware declarations it needs are hand-maintained in
`sce_decls.h` with provenance notes (verified against ps5-payload-sdk
stub symbols and the SDL port's working usage).

Status: **implemented, not executed on a console** — this is stated in
the header, README and every status table. The build path exists and is
documented in `development.md`; CI cross-builds the ELF on every push.

## 7. What replaces what (Waydroid mapping)

| Waydroid component | PS5Droid equivalent |
|---|---|
| LXC container manager | none — single-process emulation context |
| loop-mounted ext4 rootfs | read-only image inspection (ext4 probe) + build.prop parser |
| binder/binderfs | in-process Binder RPC engine (Milestone 5) + SPSC ring + block allocator |
| Wayland hwcomposer HAL | direct video-out + bitmap-font UI |
| PulseAudio audio HAL | nothing yet (explicitly unsupported) |
| D-Bus/systemd orchestration | RuntimeManager state machine |
| OTA images + sha256 | ImageManifest + Sha256 (reimplemented) |
| waydroid.prop | ImageManifest properties / BuildProp |

## 8. Roadmap (from the milestone plan)

1. **M1 done** — host foundation, tests, CI, PS5 build path (ELF-only).
2. **M2 next** — run the same probes on a real console via elfldr and
   fill the [ASSUMPTION] items in `platform-constraints.md`.
3. **M3 mostly done (v1.0.4)** — anonymous-memory subset, guest fd
   table with Linux flag semantics, `fcntl`, guest pipes (`pipe2`), readiness-based
   epoll/eventfd, `uname`, `prctl`, user/group IDs, and path-stat family demonstrated end-to-end;
   remaining: sockets, signals, directory streams, kqueue-backed poll on Orbis.
4. **M4 done at feasibility level** — bionic ELF inspection + auxv/
   initial-stack construction tested on host; full dynamic linker
   namespaces remain un-hosted.
5. **M5 done at engine level** — in-process Binder RPC engine
   (handles, transactions, block-allocated buffers) tested on host; not
   wire-compatible with the kernel protocol and never claiming to be.
6. **M6 done at inspection level (v1.0.4)** — APK structure probing (ZIP
   extraction, binary AXML AndroidManifest parsing for package name & SDK
   levels, native library ABI extraction, DEX/native-lib presence); launch research waits on M4/M5 runtime
   evidence.
