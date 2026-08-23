# Focused performance rail

Run `LAGHU_BENCH_TARGET=target-1 scripts/run-benchmarks-all` only on
`linux-fast-build` (native AMD64). The rail writes `results.json` with raw
three-run cells, medians, per-cell ratios, thresholds, exact digests, host
details, and errors. It has exactly four locked categories:

- Target 1: standalone no-optimization versus plain NGINX and Apache.
- NGINX: Laghu versus PageSpeed, five filters only.
- Apache: Laghu versus PageSpeed, five filters only.
- Standalone: all optimization versus no optimization, with NGINX upstream.

Target 1 verifies matching static delivery, virtual hosts, proxy routes, and
redirects before its load matrix. Standalone memory counts only its container,
never the shared upstream. Historical `20260821T195000Z-*` bundles are
immutable evidence and are not read or changed by this command.
