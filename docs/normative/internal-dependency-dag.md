<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Internal dependency DAG

Laghu's CMake target declarations and `target_link_libraries` calls are the authoritative internal dependency graph. This document is checked against those declarations during configuration; it is not an independent manifest.

```text
core -> none
config -> core
os -> core
protocol -> core, config, os
tls -> core, config, os
cache -> core, config, os
observability -> core, config, os
proxy -> core, config, os, protocol, tls, cache, observability
control -> core, config, os, proxy, cache, observability
cli -> core, config, control
adapters -> core, config, os, protocol, tls, cache, observability, proxy, control, cli
```

`protocol` owns the HTTP/1, HTTP/2, and HTTP/3 implementation directories. `os` owns the Linux, FreeBSD, and macOS implementation directories. Neither set of leaves is an independent graph node.

Every edge not shown above is forbidden. In particular, `core` has no outward dependencies, `os` does not depend on protocol code, no subsystem depends on `adapters`, and generic `common` or `shared` utility-sink subsystems are not part of the architecture.
