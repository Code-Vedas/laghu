<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Contributing to Laghu

Laghu is a C++23 POSIX server project configured with CMake 3.28 or newer and built with Ninja.

Contributions may include issues, bug reports, feature requests, documentation, tests, or code changes.

## Repository layout

- `src/` contains first-party subsystem code.
- `tests/` mirrors the subsystem layout.
- `cmake/` contains the normative build configuration.
- `docs/normative/` contains architecture and build contracts.
- `fuzz/` and `bench/` contain fuzzing and benchmark work.
- `scripts/` contains verified repository entry points.

## Issues

Before opening an issue:

1. Search existing issues for duplicates.
2. Use the matching issue template.
3. Include exact reproduction commands and relevant output for defects.
4. Include the POSIX operating system, architecture, compiler and version, CMake preset or configure command, build profile, and enabled features when relevant.

Do not disclose an undisclosed vulnerability in a public issue. Follow [SECURITY.md](SECURITY.md) and use GitHub Private Vulnerability Reporting.

## Pull requests

Keep each pull request focused on one coherent change and reference its related issue. Update tests and documentation when the change affects behavior, build contracts, supported features, or dependencies.

In the pull-request description, record the exact validation commands and results. State which required platform or compiler lanes were not available locally so hosted evidence remains distinct from local validation.

## Configure and build

Choose the preset matching the host:

```sh
cmake --preset linux-gcc
cmake --build build/linux-gcc
```

Available native presets are `linux-gcc`, `linux-clang`, `macos-appleclang`, and `freebsd-clang`. The `linux-aarch64-gcc` preset is compile-only and must not execute target binaries.

The direct toolchain compatibility entry point is:

```sh
scripts/check-toolchain --cxx <compiler> --out <build-directory>
```

It delegates configuration and toolchain verification to CMake and Ninja.

## Run tests

Run CTest against the configured build directory:

```sh
ctest --test-dir build/linux-gcc --output-on-failure
```

To run a focused subset:

```sh
ctest --test-dir build/linux-gcc --output-on-failure --tests-regex '<regex>'
```

Run only native tests on the host platform. Cross configurations compile targets but do not execute them.

## Documentation

Keep documentation changes consistent with the implemented behavior and the contracts under `docs/normative/`. Preserve the existing document structure and formatting when making localized edits.
