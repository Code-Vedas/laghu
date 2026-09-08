<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Internal API boundaries

Laghu does not provide a stable public C++ ABI at this milestone. There is no repository-level `include/` directory, no installed C++ headers, and no ABI export macro.

Subsystem contract headers live at `src/<subsystem>/contract/laghu/<subsystem>/`. They use `laghu::<subsystem>` namespaces and may include only their own contract headers or contract headers of dependencies allowed by the internal dependency DAG.

Subsystem-private headers live at `src/<subsystem>/private/laghu/<subsystem>/internal/`. They use `laghu::<subsystem>::internal` namespaces and are available only to sources of their owning subsystem. Private headers must not appear in another subsystem's contracts.

Contract headers must not include C headers or expose C or POSIX library types. External-library objects stay behind subsystem implementation boundaries.

All Laghu targets compile with hidden default and inline visibility. The uninstalled `laghu_visibility_probe` shared library is inspected with the configured platform `nm` tool to ensure no Laghu implementation symbol reaches the dynamic export table.
