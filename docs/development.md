# Development Guide

Current release: **v1.0.4** (see `CHANGELOG.md` and `src/version.h`).

## 1. Repository layout

See `docs/architecture.md` for the module map. Tests live in `tests/`
and use the in-repo micro framework (`tests/test_framework.h`) — no
third-party test dependencies.

## 2. Host build and test (Linux/macOS)

Prerequisites: CMake >= 3.16, a C++17 compiler (gcc or clang), Ninja or
Make.

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure   # runs the unit suite
./build/ps5droid-tests                       # direct run, verbose
./build/ps5droid-host                        # capability probes + ABI demo
./build/ps5droid-host --matrix               # full Linux ABI matrix
./build/ps5droid-host --start-attempt        # honest blocker report
./build/ps5droid-host --version              # release version
```

Sanitizer run (recommended before commits; caught real bugs in this
repo):

```sh
g++ -g -O1 -fsanitize=address,undefined -std=c++17 \
    -Isrc -Itests tests/*.cpp src/platform/*.cpp src/compat/*.cpp \
    src/android/*.cpp src/android/binder/*.cpp src/ui/*.cpp \
    -o /tmp/ps5d_tests_asan
/tmp/ps5d_tests_asan
```

## 3. PS5 payload build (requires ps5-payload-sdk)

PS5Droid targets the open-source [ps5-payload-sdk]
(https://github.com/ps5-payload-dev/sdk) (GPL-3.0; its
`include/freebsd` headers are BSD). Install it per its README (release
zip or `make install`), then:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cmake -B build-ps5 \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/toolchain/prospero.cmake \
  -DPS5DROID_BUILD_PS5=ON
cmake --build build-ps5 --target ps5droid
# produces build-ps5/ps5droid.elf  (an ELF payload, NOT a PKG)
```

**Toolchain path**: the ps5-payload-sdk *release zip* flattens the host
layout, so the toolchain is at `<sdk>/toolchain/prospero.cmake`. A
*source checkout* of the SDK keeps it under
`<sdk>/host/toolchain/prospero.cmake` — adjust the flag if you built
the SDK from source. CI uses the release-zip path.

Deploy to a console already running an ELF loader:

```sh
export PS5_HOST=<console>; export PS5_PORT=9021
$PS5_PAYLOAD_SDK/bin/prospero-deploy -h $PS5_HOST -p $PS5_PORT \
    build-ps5/ps5droid.elf
```

Without `PS5DROID_BUILD_PS5=ON` and the toolchain file the PS5 target is
skipped with a CMake status message; with the flag but no toolchain the
configure step fails loudly (no silent fake builds).

**Status reminder**: the PS5 payload compiles against verified API
usage but has not been executed on a console from this project. Treat
console results as new evidence when you get them, and record them in
`docs/platform-constraints.md`.

## 4. Coding conventions

* C++17, no exceptions thrown across module boundaries; results are
  returned as structs/enums.
* Notes returned to users (`DispatchResult::note`, blockers) must be
  static strings or stable storage — never stack buffers.
* Every syscall added to `compat/linux_abi.h` must (a) carry a support
  class and honest note, (b) have a handler or return `-ENOSYS`, and
  (c) extend the tests (`test_compat_matrix.cpp` pins the matrix
  counts; update the pinned numbers when you change the table).
* PS5 firmware API usage must cite provenance in `sce_decls.h`.
* clang-format config at `.clang-format` (LLVM-based, 4-space indent).

## 5. CI

`.github/workflows/ci.yml` runs on every push/PR:

1. **lint** — clang-format check + repository hygiene (required files,
   no stale build dirs).
2. **host** — gcc and clang builds, unit tests via CTest, logs uploaded
   as artifacts.
3. **ps5** — runs on every push and PR; installs the official ps5-payload-sdk release,
   cross-builds `ps5droid.elf`, and uploads it with an explicit "ELF payload, not a
   PKG" note. It never signs packages.

## 6. Honesty policy (project rules)

1. No fake SDK headers, no placeholder successes, no fabricated results.
2. Features are classified: *implemented and tested (host)* /
   *implemented, not tested on console* / *experimental* / *not
   implemented* / *blocked*. Every status table in the repo uses these.
3. Anything that cannot be done returns an error with an explanation
   (e.g. `-ENOSYS` with a note) instead of pretending. This applies to
   values too: the APK inspector reports no package name rather than a
   plausible-looking placeholder.
4. Proprietary material (Sony SDKs, console keys, Play Services,
   Widevine, proprietary codecs) is never bundled or downloaded.

## 7. Test conventions

* One behavior per test; deterministic (no sleeps where avoidable — the
  single 1s futex wake test bounds a thread scheduling worst case).
* Tests must clean up files they create.
* The suite must stay fast (< ~5 s) and green on both gcc and clang.
