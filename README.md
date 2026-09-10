<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Laghu

Laghu is a C++23 server project.

The repository currently provides internal runtime primitives, dependency adapters, and a minimal version-reporting CLI. Laghu is POSIX-only and uses CMake 3.28 or newer with Ninja.

## Configure, build, and test

Choose the preset matching the host. For example, on Linux with GCC 14:

```sh
cmake --preset linux-gcc
cmake --build build/linux-gcc
ctest --test-dir build/linux-gcc --output-on-failure
```

Other native presets are `linux-clang`, `macos-appleclang`, and `freebsd-clang`. The `linux-aarch64-gcc` preset is compile-only and does not execute target binaries.

Stage an installation with the standard CMake interface:

```sh
DESTDIR=/tmp/laghu-stage cmake --install build/linux-gcc --prefix /usr/local
```

The installed CLI currently supports `laghu version`, `laghu version --verbose`, and `laghu version --json`. This is not yet a runnable server interface.

## Repository layout

- `src/`: first-party implementation boundaries.
- `tests/`: subsystem-mirrored tests.
- `cmake/`: normative configuration, feature, dependency, and validation logic.
- `scripts/`: the toolchain configuration and verification entry point.
- `fuzz/` and `bench/`: fuzzing and benchmark work areas.
- `packaging/`, `examples/`, and `docs/normative/`: delivery, examples, and normative documentation.

## Licensing

Project-authored files are licensed under the GNU Affero General Public License version 3 only (AGPL-3.0-only), unless a file explicitly declares a different license.

Standalone sample configuration files, when introduced, are licensed under 0BSD and explicitly declare that license.
