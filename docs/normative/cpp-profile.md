<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Restricted systems-C++ profile

Laghu-owned source in `src/core`, `src/config`, `src/protocol`, `src/tls`,
`src/proxy`, `src/cache`, `src/control`, `src/cli`, `src/observability`, and
`src/os` is compiled as C++23 with compiler extensions disabled,
`-fno-exceptions`, and `-fno-rtti`.

CMake owns the governed target options and verifies that its configured Laghu
targets cannot re-enable exceptions or RTTI. A later `-fexceptions` or
`-frtti` is a profile violation.

Governed code uses value semantics, RAII, composition, strong types, and
zero-cost abstractions. It must not use exception handling, RTTI, coroutines,
futures or promises, `<iostream>`, or direct `virtual` declarations. The
focused lexical gate enforces these rules before compilation.

`src/adapters` is outside this lexical profile. It is the future boundary for
third-party code that cannot meet the profile; such code is isolated behind
adapter-owned interfaces rather than relaxing rules for Laghu-owned code.
