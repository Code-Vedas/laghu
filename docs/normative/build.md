<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Build configuration

Laghu uses CMake 3.28 or newer as its sole normative configure and build
authority. Ninja is the canonical generator. In-source configuration is
rejected. Configure an out-of-tree build
with a supported C++ compiler:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build --target laghu_verify_toolchain
```

`scripts/check-toolchain --cxx <compiler> --out <build-directory>` remains a
compatibility entry point. It delegates only to CMake's Ninja configure and
the `laghu_verify_toolchain` target; it does not establish compiler policy,
run probes, or select warnings independently.

Configuration requires C++23 without compiler extensions, the Laghu
restricted C++ profile, the configured warning gates, the required C++23
facilities, and the POSIX.1-2008 server baseline. CMake compile/link probes
are authoritative and retain sanitized diagnostics in `probes/` in the build
directory. Generated configuration JSON is likewise build-local and
deterministic.

Linux and FreeBSD require `_POSIX_VERSION >= 200809L`. Darwin reports the
older 2001 revision even though its supported SDK supplies Laghu's required
server APIs. On Darwin only, the numeric macro is exempt; the socket,
`bind`, `listen`, `accept`, nonblocking `fcntl`, `poll`, `close`, and
`clock_gettime(CLOCK_MONOTONIC)` compile/link probes remain mandatory. The
generated metadata records this as `darwin_server_api_exception`.

Cross configurations use compile and link probes only; Laghu's configure step
never executes a target binary.

The earlier hand-written Make wording in task #23 is superseded by this CMake
contract. Task #23 retains its out-of-tree build behavior, but does not
introduce a second configure system.

## Build targets and installation

The canonical private archive target is `laghu_core`. `laghu_core_smoke` is a
native, non-installed smoke executable. `laghu_verify_toolchain` builds both
and runs the configured validation suite. All generated files, object files,
archives, executables, probe logs, and metadata remain in the chosen binary
directory, so independent binary directories may build concurrently.

Install with standard CMake prefix and staging semantics:

```sh
cmake --install build --prefix /usr/local
DESTDIR=/tmp/laghu-stage cmake --install build --prefix /usr/local
```

The sole installed payload at this milestone is
`lib/laghu/liblaghu_core.a`; headers and `laghu_core_smoke` are not installed.
`cmake --build build --target clean` removes generated products from that
binary directory only and never mutates source files.

## Presets and ARM64 cross-builds

`CMakePresets.json` provides the unversioned `linux-gcc`, `linux-clang`,
`macos-appleclang`, `freebsd-clang`, and `linux-aarch64-gcc` configure
presets. Repository release metadata owns versioning; preset names do not
repeat it.

The ARM64 preset uses `cmake/toolchains/aarch64-linux-gnu.cmake` and the
Ubuntu package `g++-14-aarch64-linux-gnu`, which provides
`aarch64-linux-gnu-g++-14`, `aarch64-linux-gnu-ar`, and
`aarch64-linux-gnu-ranlib`. Its `try_compile` checks create static libraries,
and no cross-built executable is run.
