<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Build configuration

Laghu uses CMake 3.28 or newer as its sole normative configure and build
authority. Ninja is the canonical generator. Configure an out-of-tree build
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
