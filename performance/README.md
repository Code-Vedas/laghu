# Focused performance rail

Run `LAGHU_BENCH_TARGET=target-1 scripts/run-benchmarks-all` on
`linux-fast-build` for native AMD64 acceptance evidence. The rail also supports
native Linux/arm64 containers on local macOS Docker Desktop for Target-1
iteration only. ARM output is explicitly diagnostic-only and must never be
used as native AMD64 acceptance evidence. The rail writes `results.json` with
raw three-run cells, medians, per-cell ratios, thresholds, exact digests, host
details, container architecture, acceptance eligibility, and errors. It has
exactly four locked categories:

- Target 1: standalone no-optimization versus plain NGINX and Apache.
- NGINX: Laghu versus PageSpeed, five filters only.
- Apache: Laghu versus PageSpeed, five filters only.
- Standalone: all optimization versus no optimization, with NGINX upstream.

Target 1 verifies matching static delivery, virtual hosts, proxy routes, and
redirects before its load matrix. Standalone memory counts only its container,
never the shared upstream. Historical `20260821T195000Z-*` bundles are
immutable evidence and are not read or changed by this command.

Memory fields use explicit v2 semantics. `lifetime_cgroup_peak_bytes` is the
container-lifetime cgroup high-water (including startup) and remains the
product-footprint gate. `trial_cgroup_current_peak_bytes` samples cgroup
`memory.current` every 50 ms from immediately before k6 starts through its
completion and is the workload-footprint gate. `cgroup_peak_bytes` remains a
compatibility alias for `lifetime_cgroup_peak_bytes`. The runner never resets
`memory.peak`: on cgroup v2, reset state is specific to an open file
descriptor. For a target with multiple measured containers, trial memory is
the sum of their sampled peaks; that is exact for current single-container
targets and a conservative bound otherwise. RSS remains its historical
before/after live-process snapshot metric, not a sampled trial peak.

The retained 30-second soak uses a common two-minute post-duration drain
window. It does not extend the active load phase; it prevents one target's
queued in-flight requests from becoming client-cancelled errors.

Target 1 retains the locked-control rail and adds two diagnostic dimensions;
select either with `LAGHU_BENCH_RAIL`:

- `normalized-single-core`: every Target-1 serving container has a one-CPU
  quota; Laghu and NGINX use one worker and Apache has one bounded event-MPM
  process. The quota is the equivalence mechanism; Apache's bounded 1,024-thread
  event envelope supplies retained 1,000-VU connection capacity, not CPU.
- `production-scaling`: every Target-1 serving container has a four-CPU quota;
  Laghu has four workers, NGINX remains `worker_processes auto`, and Apache
  uses a bounded four-process, 1,024-worker event MPM. Its worker and
  asynchronous-connection envelope supports the retained 1,000-VU matrix; the
  CPU quota remains the scaling-equivalence control.

Target-1 logging is matched: NGINX uses `access_log off`, Apache disables its
default virtual-host access log, and standalone sets `runtime.access_log: off`.
Standalone lifecycle and error diagnostics remain on stderr; only request
transaction records are disabled.
Standalone no-optimization is policy-derived **production passthrough**:
when no transform, RUM, cache, or administrative feature is enabled, it starts
without transform cache, image queue, or RUM infrastructure. `minimal` uses
the same valid lifecycle; the named rail remains `production` for continuity.

On Linux AMD64, k6 uses host networking and published loopback ports. On local
ARM Docker Desktop, k6 joins the Compose network and resolves service DNS
names, while host-side contract probes retain published loopback ports. This
avoids assuming that a k6 container's `--network host` reaches macOS loopback.
`grafana/k6:0.57.0` has native `linux/amd64` and `linux/arm64` images. The
runner verifies target-container platforms and records
`native_amd64_acceptance: false` for local ARM output. Because standalone
currently closes each response, the ARM k6 client records an expanded ephemeral
port range and TCP time-wait reuse for every target; this prevents client-port
exhaustion from invalidating the retained 1,000-VU soak and is not a server
setting.
