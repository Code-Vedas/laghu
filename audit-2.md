# Laghu audit — Pass 2

I re-audited the current committed tree at `62e6b62205ddf0c85611f090d684492342757c2d`, using `audit-1.md` as the baseline. I kept **Target 1 only** for performance; I did not pull the Target-4 optimization work back into scope merely because those paths still exist. Your retained Target-1 evidence remains 0/11 against NGINX and 4/11 against Apache, with NGINX ratios of 33.4–132.5% RPS, 236.4–345.3% cgroup peak and 113.8–114.9% RSS.

This was a fresh **source + advisory audit**. I did not manufacture a new 99-trial performance bundle because I do not have the `linux-fast-build` execution environment here. The RPS conclusions below therefore use your retained native measurements and the new/current source.

The main conclusion has changed substantially from audit 1:

> **Target 1 is no longer primarily an HTTP architecture problem. The highest-value remaining work is fixed runtime memory, lifecycle initialization and mutex/request bookkeeping. There is also a measurement issue: your reported `cgroup_peak` is a container-lifetime high-water mark, not a per-trial peak.**
>
> The current blocking architecture can still eventually limit Laghu, but I would not restart the rejected keep-alive/event-loop work yet.

## 1. Target-1 performance rerun

| Priority | Finding                                                            |                 RPS |                         Memory | Action                        |
| -------- | ------------------------------------------------------------------ | ------------------: | -----------------------------: | ----------------------------- |
| **P0**   | cgroup metric is lifetime peak, not per-trial peak                 |                   — | **Critical measurement issue** | Fix rail semantics            |
| **P0**   | Passthrough creates/touches ~40 MiB unused image queue             |                 Low |                    **Extreme** | Remove from passthrough       |
| **P0**   | Cache + worker queue + RUM are mandatory even in passthrough       |                 Low |                       **High** | Feature-derived lifecycle     |
| **P0**   | `laghu_proxy_options` is multi-MiB fixed-capacity state            |                 Low |                   **High RSS** | Compact/dynamic config        |
| **P1**   | Old reload snapshots live until shutdown                           |                   — |     High over repeated reloads | Epoch/refcount reclamation    |
| **P1**   | One global proxy mutex covers too many domains                     |            **High** |                            Low | Split/atomics                 |
| **P1**   | Queue attachment maintenance runs in accept loop                   |              Medium |                              — | Move to lifecycle events      |
| **P1**   | `access_log off` still pays request bookkeeping                    |              Medium |                          Small | Fast-path it                  |
| **P1**   | Static path reopens document root and copies through userspace     |              Medium |                         Medium | Preopen root; retest sendfile |
| **P1**   | Large proxy/transform request state exists before static fast exit |              Medium |                         Medium | Lazy/split request path       |
| **P2**   | Scope/rules/route are independently looked up several times        |          Low/Medium |                              — | One dispatch context          |
| **P2**   | Listener accepts one socket per readiness loop                     | Medium under bursts |                              — | Batch accept                  |

### P0.1 — `cgroup_peak` needs two different meanings

The current rail reads `/sys/fs/cgroup/memory.peak` before and after each trial and takes the maximum, but it never resets that peak.

On current cgroup v2, `memory.peak` is the maximum since cgroup creation or the most recent reset **for that particular open file descriptor**. A reset only affects subsequent reads through that same FD. ([Kernel Archives][1])

So today a trial effectively reports:

```text
container starts
     │
     ├── queues/cache/runtime initialize
     │        ↑
     │      peak
     │
trial 1 ─────────────────┐
trial 2 ─────────────────┼── all inherit earlier high-water
trial 3 ─────────────────┘
```

This is not useless. In fact, **lifetime peak is a legitimate product-memory metric** if your contract is “Laghu must never use >102% NGINX memory, including startup.”

But it is **not a per-cell workload peak** and should not be called/used as one.

I would retain both:

```text
lifetime_cgroup_peak
    includes startup/init
    product footprint gate

trial_cgroup_peak
    reset/sampled around each trial
    request workload gate
```

Because reset semantics are FD-specific, don't just run a separate `echo > memory.peak` followed later by another `cat`. Either hold one FD open across reset→trial→read in a helper, or independently sample `memory.current` throughout the trial.

This matters especially for patches such as the rejected `sendfile` experiment. That candidate gained roughly 25–36% plaintext RPS but was discarded because cgroup supposedly increased ~14.9% while RSS was essentially unchanged.  **I would retest that candidate after separating lifetime and trial memory.**

Do not automatically restore it; just reopen the evidence.

---

## 2. P0: Target 1 unnecessarily creates a ~40 MiB image queue

This is the clearest new fixed-memory offender.

The benchmark entrypoint initializes `laghu-libvips` even for passthrough and always configures a worker queue.

The image worker initializes its queue using:

* default slots = **4**
* per-slot payload = `LAGHU_IMAGE_MAX_INPUT_BYTES`
* image max input = **10 MiB**

and calls `laghu_runtime_queue_create()`.

Queue creation maps the complete file and zeroes the complete mapped length. That touches/faults the payload pages rather than merely reserving sparse capacity.

Therefore Target 1 effectively does:

```text
4 × 10 MiB
≈ 40 MiB payload mapping
+ queue headers
```

for a queue that **passthrough can never use**.

This is almost tailor-made to explain the enormous cgroup ratio in the current result.

### Fix both layers

First, passthrough should not require an image worker queue at all.

Second, even when a queue is required, queue creation should not `memset()` the entire payload area. Initialize the queue header and each slot header/state. Empty-slot payload bytes are irrelevant until a slot becomes READY with a validated length.

That avoids eagerly dirtying/touching tens of MiB.

---

## 3. P0: the requirement itself is currently hard-coded

This isn't merely a benchmark-entrypoint problem.

Standalone YAML finalization explicitly sets:

```c
.require_cache = true
.require_worker_queue = true
```

for every scope, even though it has already resolved the Laghu policy.

Then startup unconditionally creates RUM, probes the cache, validates the worker queue, attaches queues and registers the cache backend.

So the architecture currently says:

```text
rewrite_level: passthrough
           │
           ├── RUM engine
           ├── image cache
           ├── image worker queue
           └── cache backend
```

even though policy says none of those transformation facilities will participate in an ordinary Target-1 response.

I would make lifecycle requirements derive from the **resolved effective policy plus explicitly enabled administrative features**.

For example:

```text
passthrough
  no cache administration
  no optimization
  no RUM dependent feature
      ↓
no transform cache
no image queue
no RUM engine
```

This must be a real product mode, not a benchmark-only conditional.

This is now my **#1 product change after correcting the rail**.

---

# 4. P0: `laghu_proxy_options` is probably most of the 14–15% RSS gap

The retained data is striking:

* Laghu RSS ≈ 54.8–55.5 MiB
* NGINX RSS ≈ 47.9–48.8 MiB
* delta only about 6–7 MiB

Now look at the configuration representation.

`laghu_proxy_options` embeds **32 sites and 64 routes** at fixed capacity. Each site/route embeds full `laghu_config`, `laghu_service_config` and `laghu_proxy_rules`.

A `laghu_service_config` alone has roughly twenty-two 1,024-byte path buffers plus other state.

`laghu_config` includes the 2 KiB MIME allowlist, resource rule arrays and domain-policy state.

The basic-auth rules alone can contain 64 usernames + hashes.

Yet Target 1 uses **two sites and two routes**.

Worse, `main()` declares the complete `laghu_proxy_options` on its stack, and initialization zeroes/initializes the full fixed-capacity object, touching pages for unused site/route slots.

The visible embedded arrays give a **multi-megabyte fixed footprint** before normal runtime allocations. That fits the measured RSS delta unusually well.

### Replace fixed-capacity runtime representation

Parse into temporary builder structures, validate bounds, and then compile to:

```text
laghu_proxy_options
    sites  -> exact-count allocation
    routes -> exact-count allocation

site
    host
    root
    pointers/indices to immutable scopes

route
    match data
    upstream
    pointer/index to immutable scope
```

Do not repeat full `config + service + rules` blobs for every route if multiple objects share resolved state.

Moving the same enormous struct from stack to heap alone is not enough; **compact it**.

This should also materially improve CPU-cache locality.

---

# 5. Reloads currently accumulate those giant snapshots

This is not exercised by Target-1 benchmarking, but it is directly relevant to standalone memory.

A successful reload allocates another complete `proxy_reload_snapshot`, puts it on a linked list and switches `queue->options` to it. Old snapshots are only destroyed during final shutdown.

The lifetime looks like:

```text
initial config
reload #1  ─ retained
reload #2  ─ retained
reload #3  ─ retained
...
shutdown   ─ finally all freed
```

That lifetime is understandable—the code is protecting request workers that may still borrow an old immutable snapshot—but with the current multi-MiB representation it makes reload memory monotonically grow.

Once the configuration becomes compact, use epoch/refcount/RCU-style reclamation and free an old generation after no request can hold it.

---

# 6. Target-1 RPS work after fixed memory

I would **not** jump back into the event-loop rewrite yet.

The current pthread pool still has contention worth removing first.

`queue_push`, `queue_pop`, active request tracking, worker active sockets and several runtime readiness/accessors share the proxy lock. Worker completion also broadcasts `drained` on ordinary request completion.

Additionally, the listener repeatedly calls queue-attachment maintenance even when those queues are already attached. That function itself checks state under the global lock.

I would split state into:

```text
connection queue mutex
reload/config generation
lifecycle/drain state
origin-pool state
runtime attachment state
```

and make cheap state atomic where appropriate:

`active_count`, request sequence, stop/forcing state, attachment-ready booleans.

Only broadcast `drained` while actually draining/waiting.

### `access_log off` can go further

Your retained change correctly prevents formatted transaction output, mutex/write/flush work and produced a material performance gain.

But request initialization still has bookkeeping: timers, IDs and operational state. There should be a genuinely cheap “nothing consumes this telemetry” path.

Generate tracing IDs only when access logging/tracing needs them.

---

# 7. Static serving still has another iteration available

The retained mode-off bypass was the correct change.

But static serving still opens the configured document root and performs component-wise `openat()` traversal for each request. It then reads regular file data into a userspace buffer and sends it.

The safe `O_NOFOLLOW` traversal should remain.

The next safe optimization is:

```text
config/reload
    ↓
open + validate document-root fd once

request
    ↓
borrow root fd
    ↓
openat beneath it
```

That removes one pathname resolution/open per request without weakening traversal protection.

Then retest the discarded fd/sendfile branch with corrected memory instrumentation.

---

# 8. Don't re-open the rejected work indiscriminately

I agree with the dispositions captured in `audit-1.md`.

The keep-alive/repark candidate regressed static RPS ~61%; leave it rejected for now. The regex precompile candidate improved regex matching but hurt normal static/prefix traffic; leave it rejected. GCC IPO/LTO and PGO regressed measured RPS; leave them rejected. jemalloc independently increased **RSS** by roughly 22–28%, so the cgroup-metric issue does not rescue it.

Only one discarded experiment deserves immediate reconsideration: **the static fd/sendfile candidate**, because its strong RPS gain was rejected primarily on a cgroup increase while RSS was flat.

---

# 9. Exact Target-1 order I recommend now

1. **P2.R1 — Split memory metrics.** Retain lifetime cgroup high-water and add genuine per-trial cgroup/current peak.
2. **P2.R2 — Remove transform infrastructure from true passthrough.** Cache, worker queue and RUM become feature-derived requirements.
3. **P2.R3 — Stop touching empty queue payload space.** Initialize metadata/slot headers only.
4. **P2.R4 — Compact `laghu_proxy_options`.** Exact site/route allocations and shared immutable scopes.
5. **P2.R5 — Rerun native Target 1.** This becomes the first meaningful post-audit-2 baseline.
6. **P2.R6 — Retest the fd/sendfile candidate.** Use both lifetime and trial memory metrics.
7. **P2.R7 — Split the global proxy mutex.** Atomics for counters/state where safe.
8. **P2.R8 — Remove queue attachment work from normal accept-loop iterations.**
9. **P2.R9 — Preopen document roots and retain secure `openat` traversal.**
10. **P2.R10 — Lazy static request state / one dispatch context.** Don't initialize proxy/transform state until static/redirect handling actually misses.
11. **P2.R11 — `perf`/flamegraph the new native baseline.** Only then decide whether a larger transport/event-loop change is warranted.

I would freeze Target-4 work until these are measured.

---

# 10. Security audit

There are several good foundations in the current code.

Origin TLS requires TLS 1.2+, enables peer verification, verifies hostname/IP and disables TLS compression/renegotiation.

The static file path remains symlink-conscious and traversal-conscious. The font/resource fetcher is HTTPS-only, checks DNS results for public addresses before connecting, and performs TLS hostname verification.

CI already has ASan + UBSan and libFuzzer runs for image/runtime code.

I nevertheless found these offenders:

| Severity             | Finding                                                         | Exposure                    |
| -------------------- | --------------------------------------------------------------- | --------------------------- |
| **HIGH**             | Slowloris / no absolute header-body deadline                    | Remote availability         |
| **MEDIUM**           | Direct-TLS request bodies are read from raw socket              | TLS framing/correctness     |
| **MEDIUM**           | Request-line grammar weaker than header grammar                 | Proxy desync hardening      |
| **MEDIUM**           | Basic-auth passwords stored as unsalted SHA-256                 | Offline credential cracking |
| **MEDIUM**           | No project-guaranteed production C hardening profile            | Exploit mitigation          |
| **MEDIUM**           | Vendored libyaml lacks nesting-depth DoS fix                    | Config startup/reload DoS   |
| **MEDIUM**           | Container source/base dependencies not cryptographically pinned | Supply chain                |
| **LOW/MEDIUM**       | Purge token has path-check/open TOCTOU                          | Local attack                |
| **Conditional HIGH** | Chrome automatically uses `--no-sandbox` as root                | Browser-worker compromise   |
| **MEDIUM**           | Cargo security updates are not automated/audited                | Supply-chain maintenance    |
| **LOW/MEDIUM**       | Release Actions use mutable major tags                          | CI/release supply chain     |

## S1 — HIGH: Slowloris can consume every standalone worker

Socket `SO_RCVTIMEO/SO_SNDTIMEO` provides an **idle operation timeout**, but `proxy_read_headers()` has no absolute request deadline. As long as an attacker sends another byte before the socket idle timeout, the read loop can continue until the header-size limit.

The body reader has the same property.

With four Target-1 workers:

```text
attacker 1 ─ slow partial header ─ worker 1
attacker 2 ─ slow partial header ─ worker 2
attacker 3 ─ slow partial header ─ worker 3
attacker 4 ─ slow partial header ─ worker 4

legitimate requests → queue → eventual saturation
```

And access/rate policy isn't applied until after a complete request header has been parsed.

Add both an **idle timeout and absolute deadline**:

* header total deadline;
* body total deadline or minimum byte-rate policy;
* optionally per-peer concurrent incomplete-request limits.

This is the strongest remotely reachable security finding in this pass.

### S1 disposition — accepted

Standalone now enforces independent monotonic downstream deadlines: headers
default to 10 seconds and `Content-Length` bodies to 60 seconds; both accept
configured values from 1 through 300 seconds via
`request_header_timeout`/`request_body_timeout`. `io_timeout` remains the
per-operation idle limit; neither total deadline resets after a byte arrives.
Origin/upstream reads retain their existing timeout path.

The read loop bounds each plain or TLS wait by the smaller remaining total
deadline and idle timeout, handles `EINTR` plus TLS `WANT_READ`/`WANT_WRITE`,
then restores normal socket idle timeouts. Expiry preserves current request
failure behavior: 400, `client_parse`, close, and normal access-log handling.

Focused local evidence: parser/YAML bounds tests, standalone smoke, and
plain/downstream-TLS one-worker slow-trickle header/body regressions passed;
each timed-out worker then served normal GET and complete POST. A reload with
changed deadline fields applied successfully. ASan/UBSan proxy unit and direct
sanitizer standalone smoke passed; macOS ASan lacks leak-detector support.

---

# 11. S2 — direct downstream TLS POST body bug

This is a concrete code defect.

Headers are read correctly through:

```c
proxy_read_headers(client, ..., client_tls, ...)
```

But if the body isn't fully contained in that first decrypted read, the subsequent body call is:

```c
proxy_read_body(client, NULL, ...)
```

Passing `NULL` makes the body reader call raw `recv()` rather than `SSL_read()`.

For direct TLS, those remaining bytes are TLS records, not HTTP body bytes.

That can break POST/beacon/admin/proxy semantics or desynchronize/error the request.

The correction is straightforward:

```c
proxy_read_body(client, client_tls, ...)
```

Then add tests where the TLS body is deliberately split across multiple records/reads.

I rate this **medium security/correctness**, not a proven confidentiality break.

### S2 disposition — accepted, no additional source change

S1's retained downstream deadline reader already made the required correction:
the direct client body call receives `client_tls`, so post-header TLS bytes are
read through `SSL_read`, not raw `recv`. No further production alteration was
needed for S2.

Focused regressions send headers first and a `Content-Length` body in separate
writes/TLS records. Direct TLS proxy POST preserved the exact echoed body; a
formatted beacon POST reached its semantic `worker` failure rather than generic
body parsing; authenticated PURGE with a body returned 202. Incomplete TLS
body close released the worker for a following POST, while a slow TLS body kept
the existing 400 timeout behavior. Equivalent split plaintext and TLS POSTs
both preserved exact bodies. Source inspection confirms no
`proxy_read_body(client, NULL)` remains; the sole `recv(client, ...)` is
rejected-connection cleanup.

Parser unit, standalone smoke, and direct ASan/UBSan standalone smoke passed;
lint and diff checks passed. This validates TLS framing and direct-body read
correctness.

---

# 12. S3 — tighten request-line grammar

Header parsing is appropriately strict about token characters and control bytes.

The request line is looser. It splits method/target/version but does not subject method and target to equally strict character validation before they can later participate in an upstream HTTP request line.

You already correctly reject important ambiguity classes:

* duplicate Host;
* missing Host in HTTP/1.1;
* conflicting Content-Length;
* Content-Length + Transfer-Encoding.

So I am **not claiming a demonstrated request-smuggling exploit**.

I am saying this is a proxy desynchronization hardening gap. Enforce HTTP method `tchar` grammar and reject inappropriate C0/DEL/whitespace characters in the origin-form target before routing or forwarding.

Also add a dedicated protocol fuzz harness. Current fuzz CI covers image/runtime code, not the HTTP request/parser surface.

### S3 disposition — accepted

The standalone request parser now accepts only RFC token characters in the
method and rejects whitespace, C0 controls, and DEL in its origin-form target
before routing. Existing origin-form behavior remains unchanged: encoded paths
are opaque, `//` and query strings remain accepted, and absolute, authority,
asterisk, and fragment forms remain rejected. NGINX and Apache adapters do not
use this parser.

Focused local parser cases cover valid token methods, encoded targets, exact
separators and versions, controls, target forms, duplicate `Host`, and
`Content-Length`/`Transfer-Encoding`. A bounded standalone request-line
libFuzzer target runs both full request input and a header-completed line
fixture, checks deterministic accept/reject, and is compiled with
ASan/UBSan. The retained five-seed corpus completed 20,000 runs with 174
coverage counters and no sanitizer finding. Normal parser and proxy smoke,
sanitizer parser and standalone smoke, lint, and `git diff --check` passed.

---

# 13. S4 — Basic-auth password hashes are too cheap

The password file format is:

```text
username:sha256:<64 hex characters>
```

Login takes the submitted password and computes a single SHA-256 before constant-time digest comparison.

The constant-time digest comparison is good.

The storage KDF is not.

If the auth file leaks, a GPU can test enormous password dictionaries cheaply because there is no random salt and no expensive password KDF.

Use a versioned format with something such as **scrypt** or **Argon2id**, including salt and work parameters.

Also make the deployment contract explicit: HTTP Basic credentials must be under direct TLS or a specifically trusted TLS-terminating proxy.

### S4 disposition — accepted

New credentials use the fixed versioned format
`username:scrypt-v1:16384:8:1:<16-byte-salt-hex>:<32-byte-derived-key-hex>`.
OpenSSL `EVP_PBE_scrypt` derives the key with a 32 MiB maximum-memory bound;
the parser accepts only those exact `N`, `r`, and `p` values, preventing an
auth-file from selecting unbounded work. Derived-key comparison is
`CRYPTO_memcmp`, and decoded credentials and transient derived keys are cleared.

Legacy `username:sha256:<64-hex>` entries remain read-only for an explicit
migration period and are documented as deprecated. Laghu never rewrites a
password file. Basic authentication now requires direct TLS, or an `https`
forwarded scheme from an explicitly configured trusted TLS terminator.

Focused tests cover a deterministic scrypt vector, successful and rejected
credentials, clear-HTTP denial, trusted-terminator success, malformed base64
and hex, fixed-parameter bounds including extreme values, duplicate users,
legacy loading, and existing secure-mode/symlink file checks. Normal auth unit
and proxy smoke, ASan/UBSan unit and standalone smoke, lint, and
`git diff --check` passed.

---

# 14. S5 — purge-token TOCTOU

`proxy_admin_token()` does an `lstat()` security check and later opens the same pathname with `fopen()`.

The password-file code already demonstrates the stronger pattern: `lstat → open → fstat`, comparing device/inode and checking the opened object.

Use that same pattern for the purge token with `O_NOFOLLOW | O_CLOEXEC`.

This is primarily local hardening because exploitation requires control over the relevant path namespace.

### S5 disposition — accepted

`proxy_admin_token_file_read()` now uses `lstat → open(O_RDONLY|O_NOFOLLOW|O_CLOEXEC) → fstat`, requires matching device/inode, regular type, effective owner, and private mode before reading at most 256 bytes from the opened descriptor. POSIX fallbacks retain the inode check; missing `O_CLOEXEC` is set with `fcntl` before validation. Token trimming, comparison, errors, and cleanup remain unchanged.

Focused tests cover valid/wrong token, unsafe mode, symlink, directory, FIFO, empty/oversized files, replacement before open, and deletion after open. `laghu_proxy_test`, proxy smoke, ASan/UBSan unit plus standalone smoke, lint, and `git diff --check` passed.

---

# 15. S6 — Chrome must never silently disable its sandbox

The Chrome worker currently does:

```c
root ? "--no-sandbox" : NULL
```

Your packaged systemd service mitigates this correctly by running as `User=laghu`, with `NoNewPrivileges`, `PrivateTmp`, `ProtectSystem`, and `ProtectHome`.

So the shipped service is not the offender.

The problem is manual/root execution: Laghu reacts to excessive privilege by disabling Chromium's sandbox.

I would invert that:

```text
geteuid() == 0
      ↓
refuse startup
```

unless an explicit, separately reviewed privilege-dropping mechanism is used.

### S6 disposition — accepted

`laghu-chrome-analyze` now refuses every valid startup mode when its effective UID is root, before queue creation or worker readiness, writing a clear diagnostic and exiting `77`. The Chrome argv has no sandbox-disabling flag; normal non-root invocation remains unchanged. The NGINX container already runs as `laghu`, Apache uses `runuser -u laghu`, and the packaged systemd unit retains `User=laghu` while treating exit `77` as non-restartable.

Focused startup tests use a test-only mocked root EUID plus real non-root initialization to verify the exit, diagnostic, absent queue, normal queue creation, argv ban, and systemd contract. Normal and ASan/UBSan CTest, lint, and `git diff --check` passed.

---

# 16. CVE/advisory audit — current status

### Vendored libyaml nesting-depth disposition — accepted

The vendored 0.2.5 identity and MIT license remain unchanged. Because upstream
still publishes only pre-release `0.2.6-rc.1`, Laghu backports the exact
1,000-level default from `51843fe48257c6b7b6e70cdec1db634f64a40818` and its
combined flow/block scanner completion from
`849d0aefecb42fe70165ed1557c329aff9e74d3e`; parser errors now report the
upstream `exceeded maximum nesting depth` diagnostic.

Vendor tests accept near-limit flow/block documents and aliases, reject depth
1,001, and exercise the error. Standalone startup and `reload --config` reject
deep YAML within five seconds without stopping the active process. Normal and
ASan/UBSan vendor/YAML/proxy tests plus proxy smoke, lint/license headers, and
`git diff --check` passed.

### Cargo dependency/advisory disposition — accepted

The sole Cargo root now requires `anyhow >=1.0.103` and `memmap2 >=0.9.11`, while retaining the already-patched committed lockfile. Weekly Cargo Dependabot covers that root; CI installs pinned `cargo-audit 0.22.2`, verifies that `Cargo.lock` is tracked, and audits that exact file.

All 16 Rust tests passed with `--locked`; `cargo-audit 0.22.2 --file Cargo.lock` scanned all 228 locked crates clean. The Cargo-root/workflow contract, lint/license/format checks, and `git diff --check` passed.

### Vendored libyaml: actionable

Laghu vendors **libyaml 0.2.5**.

Upstream's current `0.2.6-rc.1` says explicitly that it fixes a **denial-of-service vulnerability by limiting nesting depth**.

As of August 24, 2026, upstream still labels 0.2.5 as the latest stable release and 0.2.6-rc.1 as prerelease. ([GitHub][2])

This is not network-request YAML in Laghu, so normal remote HTTP clients don't directly trigger it. But startup/reload parses configuration YAML, meaning an excessively nested malicious or mistaken config can consume pathological parser resources.

I would backport/review the current upstream nesting-limit fix now and add a deliberately deeply nested configuration test. Move to final 0.2.6 when released.

Also, don't let automated scanners mislead you about old libyaml reports:

**CVE-2024-35328 is REJECTED.** ([NVD][3])
**CVE-2024-3205 is REJECTED.** ([NVD][4])

They should not appear as active Laghu vulnerabilities.

### Rust: current known advisories I checked are patched

Current `Cargo.lock` contains:

```text
anyhow 1.0.104
```

RustSec's `RUSTSEC-2026-0190` affects `anyhow <1.0.103`; your lock is therefore patched. ([RustSec][5])

Your lock contains **memmap2 0.9.11**.  RustSec `RUSTSEC-2026-0186` affects versions below 0.9.11 and identifies 0.9.11 as the patched release. ([RustSec][6])

However, `Cargo.toml` still specifies `memmap2 = "0.9.9"`, so I would raise its lower bound to 0.9.11.

More importantly, Dependabot currently covers only Bundler docs and GitHub Actions; there is **no Cargo ecosystem entry**.

Add:

```text
Cargo Dependabot
+ cargo audit --locked
or cargo-deny advisories
```

to CI.

I would **not stamp the complete Rust dependency graph “0 vulnerabilities”** until that machine-generated lockfile audit is running. The source/advisory check above verifies the relevant current advisories I located; it is not a substitute for continuously evaluating every transitive crate.

### NGINX 1.30.4: currently good

Laghu's packaging uses NGINX 1.30.4.

Official NGINX advisories list 1.30.4 as not vulnerable to the July 2026 `map/regex`, slice-module and SSI issues, among the other 2026 fixed branches. ([Nginx][7])

NGINX also currently lists **1.30.4 as the stable release**. ([Nginx][8])

So I found no reason to change that version today.

---

# 17. CVE coverage is incomplete at the OCI image level

This distinction matters.

The container build installs OpenSSL, libvips and other distro packages via `apt` during image construction and uses mutable base-image tags. Source inspection therefore cannot tell me the exact installed package builds in a particular GHCR image. The same Dockerfile rebuilt tomorrow can resolve different packages.

Consequently, a complete statement such as:

> “Laghu has zero CVEs”

would be unsupported.

You need to scan the **actual resulting OCI digest**, not merely source manifests.

Add a release gate using Trivy/Grype/OSV-compatible scanning against the final image/SBOM and retain the scan alongside the image digest.

Your release process already creates an SBOM and signed checksum/provenance for release assets, which is a good foundation.

### OCI image CVE disposition — accepted

Release now scans each published image by its immutable build digest before promotion and retains its raw Trivy JSON with that digest. The policy records all OS/library severities, then fails only on fixed HIGH/CRITICAL findings; promotion occurs only after that gate.

Native AMD64 proof on `linux-fast-build` built and scanned both distinct final images: `ngx-laghu@sha256:650b49922de1a9d08ae51d9824482e49af234eff1c8e670ad1fc34eaf9a04516` (834 LOW/MEDIUM findings, zero HIGH/CRITICAL) and `mod-laghu@sha256:08da264cc5600728bddd0cd9cf460e33996705d6e8b237881ae90edcff554059` (789 LOW/MEDIUM, zero HIGH/CRITICAL). Both raw scans and fixed-HIGH/CRITICAL gates exited zero; raw JSON, commands, logs, and SHA-256 manifest remain at `/home/debian/laghu-oci-cve-20260902T142000Z/evidence`, after task-local registry/image cleanup.

---

# 18. Supply-chain findings

The container Dockerfile downloads NGINX source directly and builds it, but does not verify the tarball's SHA-256 or PGP signature. It also uses tagged Ubuntu/Rust images rather than immutable digests.

NGINX publishes signatures alongside 1.30.4, so this is straightforward to harden. ([Nginx][9])

Pin/review:

```text
base image digest
Rust builder digest
NGINX tarball SHA-256
NGINX PGP signature
```

Release workflows also use Actions by mutable major tags such as `actions/checkout@v7`. The release workflow has powerful write/package/id-token permissions.

Dependabot updates those Actions weekly, which is good, but for release/security workflows I would pin external Actions to full commit SHAs and let Dependabot update the SHAs.

### Immutable release inputs and Action pins disposition — accepted

Both release Dockerfiles now pin the Ubuntu 24.04 and Rust 1.95-bookworm indexes by digest; the NGINX image verifies the pinned `1.30.4` tarball SHA-256 and its official Roman Arutyunyan PGP signing fingerprint. All 58 workflow actions are full-commit pinned with version comments, with weekly Dependabot coverage for Actions and release-container images; focused contracts prevent mutable base/Rust/action refs and missing NGINX checks.

On `linux-fast-build`, the NGINX release image completed with checksum and `VALIDSIG` evidence. The distinct Apache release image also completed and its loaded `linux/amd64` tag was observed (`sha256:2409c7fac93f`, 924 MB), but the interrupted session did not reach its inspect step and the host became disk-full immediately after; that caveat is retained beside the raw build logs and checksums at `/home/debian/laghu-supply-chain-20260902T173000Z/evidence`. Only the task-owned validation image tags were removed; no broad prune occurred.

---

# 19. Production C hardening is not guaranteed by Laghu itself

The build has good sanitizer support, but the normal CMake project does not itself specify a hardened production profile.

Distro packaging can inject some protections, so this is **not proof that every emitted binary lacks PIE/RELRO/etc.**

It means Laghu does not guarantee them.

Add a release hardening profile and verify the resulting binaries, rather than merely setting compiler flags and assuming they stuck. Baseline targets should include stack protector, FORTIFY where supported, PIE for executables and RELRO/NOW.

Benchmark the hardening build to make sure Target-1 measurements use the same release characteristics you actually ship.

### Production C hardening disposition — accepted

Linux Release CMake builds now require stack protector, FORTIFY=3, PIE where applicable, and RELRO/NOW; Debian, RPM, and both release containers select the profile explicitly, while Apache and NGINX module builds apply the matching C/link flags. A focused checker validates compile commands plus ELF type, GNU_RELRO, non-executable stack, NOW, and stack-protector references; CI/release module jobs run its contract/artifact checks.

On `linux-fast-build`, nine native AMD64 Release artifacts passed: standalone, six C workers, Apache module, and NGINX 1.30.4 module. The standalone is a DYN PIE with `FLAGS_1 NOW PIE`; both modules are DYN with RELRO/NOW and a non-executable stack. Focused contracts, workflow YAML parse, lint/license, `laghu --help`, and `git diff --check` passed. Logs and SHA-256 manifest remain at `/home/debian/laghu-hardening-20260902T184500Z/evidence`; only its isolated source/build/module directories were removed.

---

# 20. Security CI I would add now

The existing CodeQL job analyzes C/C++, and sanitizer/fuzz CI covers image/runtime.

The highest-value additions are:

```text
cargo audit --locked / cargo-deny
OCI image CVE scan by final digest
HTTP protocol/request parser fuzz harness
chunk parser fuzz corpus
deep-YAML nesting regression
Slowloris absolute-deadline integration test
direct-TLS split-body POST integration test
binary hardening check
release action SHA pinning
```

Of those, **Slowloris** and the **direct-TLS body bug** are the two source-security fixes I would make before broadening the audit further.

### Chunked-parser corpus / fuzz disposition — accepted

The five in-repository chunked fixtures remain covered by the deterministic
`laghu_proxy_chunked_regression` CTest: native Linux/AMD64 Clang 19.1.7 ran
1/1 passing in 0.05 seconds. The existing `laghu_proxy_chunked_fuzz` target
was compiled with its configured `-fsanitize=fuzzer,address,undefined` flags
and run locally with `-runs=20000 -seed=20260902` against only
`servers/laghu/tests/corpus/chunked`: exit 0, 20,000 completed iterations,
and 1.503 seconds elapsed, with no ASan/UBSan finding. The harness allows
malformed, truncated, and trailing inputs to be rejected while asserting that
every successful decode owns a bounded body and that two successful feed
shapes produce identical bytes. Raw build, CTest, status, and fuzz logs are
retained at `/home/debian/laghu-chunked-fuzz-20260902T220000Z/evidence/`
(`chunked-fuzz-build-r2.log`, `chunked-regression-ctest-r2.log`, and
`chunked-fuzz-linux-clang19-20000-r2.log`); the task-local Clang build used
`-Wno-error=newline-eof` solely for the pre-existing missing final newline in
vendored `third_party/libyaml/src/loader.c`.

---

# 21. Overall Pass-2 disposition

The work after audit 1 was productive. The incremental chunk parser is retained and substantially better; static mode-off now genuinely avoids the shared transformation finalizer; access logging no longer poisons Target-1 comparison; the benchmark CPU topology is much more defensible.

The next milestone should **not** be “make Laghu event-driven.”

It should be:

> **Make passthrough truly small.**

Specifically, after startup Target-1 Laghu should contain little beyond:

```text
listener
4 worker threads
2 sites
2 routes
minimal immutable config
small connection queue
static/proxy transport state
```

No 40 MiB image queue. No transformation cache unless needed. No RUM unless needed. No 32-site/64-route fully materialized structures when only 2+2 exist.

Once that is done, rerun the native 99 trials. I expect that result to tell us much more clearly whether the remaining RPS problem is mutex/system-call overhead or whether the transport architecture itself has become the next limiting factor.

For security, I would classify **Slowloris as the highest-priority remote offender**, followed immediately by the **direct-TLS body-read bug**. For dependency security, the only current source-level advisory I found that requires action is the **vendored libyaml nesting DoS**; the checked Rust advisories are already patched and NGINX 1.30.4 is current/fixed.

---

## 22. Measured P2 disposition — native Target-1 rerun

Retained changes, now measured together:

* **P2.R1 — retained:** v2 rail records lifetime `memory.peak` separately from
  50 ms sampled trial `memory.current`; both cgroup gates and RSS are applied.
* **P2.R2 — retained:** true passthrough derives its lifecycle from effective
  policy and starts no transform cache, image/worker queue, or RUM facility.
* **P2.R4 — retained:** sites/routes use bounded exact-count backing rather
  than materializing 32 sites and 64 routes. Target 1's two sites/two routes
  reduce the modeled config footprint from 7,438,904 B to 390,808 B.

The complete native AMD64 `production-scaling` bundle is
`/home/debian/laghu-target1-99.NJZAaI/bundle`. It contains 99 raw runs
(standalone, plain NGINX, plain Apache; 11 cells; 3 independent runs/cell),
33 medians, exact config and image digests, corpus digest, and cgroup-v2 v2
metadata. The run was eligible native evidence (`linux-fast-build`,
`linux/amd64` containers, four-CPU quota, four standalone workers, matched
access-logging control); all 99 runs had zero HTTP/check errors. `results.json`
has `errors: []`; there were no invalid attempts to retain.

Each compact cell below is `RPS / lifetime-cgroup / trial-cgroup / RSS`, as a
percentage of the plain-server baseline. A pass requires RPS >=98% and every
memory value <=102%.

| Cell | Standalone vs NGINX | Standalone vs Apache |
| --- | --- | --- |
| warm 1 | 131.3 / 56.3 / 51.7 / 62.7 — pass | 223.8 / 22.4 / 17.9 / 57.9 — pass |
| warm 10 | 70.5 / 56.3 / 48.3 / 63.7 — fail | 73.4 / 20.4 / 15.4 / 54.8 — fail |
| warm 50 | 35.7 / 55.0 / 53.8 / 63.7 — fail | 55.9 / 19.6 / 16.5 / 51.0 — fail |
| warm 100 | 59.3 / 55.0 / 49.2 / 63.6 — fail | 97.5 / 17.4 / 13.7 / 44.4 — fail |
| warm 500 | 66.4 / 54.4 / 55.6 / 63.5 — fail | 85.5 / 12.1 / 11.4 / 30.2 — fail |
| warm 1000 | 78.7 / 65.7 / 62.7 / 63.6 — fail | 82.4 / 12.0 / 11.2 / 26.2 — fail |
| JavaScript 10 | 75.8 / 65.7 / 54.0 / 63.5 — fail | 83.1 / 12.0 / 8.6 / 25.5 — fail |
| mixed assets 1000 | 74.2 / 73.3 / 62.9 / 63.9 — fail | 87.1 / 13.4 / 11.0 / 25.7 — fail |
| cache storm 1000 | 89.6 / 78.0 / 65.6 / 63.7 — fail | 112.2 / 13.9 / 11.5 / 25.6 — pass |
| cache thrash 1000 | 109.7 / 76.3 / 70.4 / 66.3 — pass | 126.1 / 13.9 / 12.4 / 26.4 — pass |
| soak 1000 | 47.4 / 74.4 / 74.1 / 61.7 — fail | 63.2 / 19.7 / 15.9 / 26.3 — fail |

**Current comparison-gate status: fail.** Standalone passes **2/11** against NGINX
and **3/11** against Apache. Memory passes every compared cell: NGINX ratios
are 54.4–78.0% lifetime cgroup, 48.3–74.1% trial cgroup, and 61.7–66.3% RSS;
Apache ratios are 12.0–22.4%, 8.6–17.9%, and 25.5–57.9% respectively. RPS is
the sole blocker: 35.7–131.3% versus NGINX and 55.9–223.8% versus Apache.

The earlier retained P2.R4 native A/B bundle at
`/home/debian/laghu-p2r4-ab.mZbl2u/evidence/results.json` is only directional:
it used one standalone worker and two fixed warm cells, whereas this bundle
uses four workers and the full production-scaling matrix. It therefore cannot
establish an RPS delta. Its 2+2 exact-count memory reduction is consistent
with the present all-cell memory pass, but its numbers are not merged into
this verdict.

**Next Target-1 priority:** P1 global proxy mutex/request bookkeeping. The
P2 work has removed the memory blocker; the full matrix now isolates
throughput under concurrency as the blocker. Start with a narrow contention
probe around the global proxy lock and request-completion/drain path, then
propose one measured change; keep R3 and reload reclamation out of that cycle.

---

## 23. Retained/rejected disposition and current native Target-1 proof

The section 22 bundle is superseded for the current accepted tree by the
native AMD64 `production-scaling` rerun at
`/home/debian/laghu-target1-99.cyYCS5/bundle/results.json`
(`sha256: 108771937184eb8c18ce97356211f7e2299875962c5be8798db1cd1d14ea5afe`).
It contains 99 raw trials, 33 medians, 22 cell-by-cell comparison verdicts,
and zero recorded HTTP/check errors. The source/image/config digests are in
the bundle; the isolated source provenance is
`/home/debian/laghu-target1-99.cyYCS5/source-digests.sha256`
(`sha256: 6d10fd1bbae82dd8bdae94a1d0b3711bf7642c0341e44a7fbe98fc12456573ed`).

Retained after focused native evidence:

* **P1 dead drained-broadcast removal:** eliminates a proved no-waiter
  completion broadcast; retained without changing completion semantics.
* **P2.R9 preopened roots:** secure `O_NOFOLLOW`/`openat` traversal remains;
  focused native A/B improved warm-50 RPS 20.8% and mixed-assets 24.5%, with
  no material RSS regression.
* **P2.R11 response-header batching:** native `strace` proved eight sends for
  one passthrough static response (status, six headers, body) and only 4–5
  futex calls per 1,000 requests. Header bytes are now sent as one bounded
  buffer; the body path is unchanged. The direct trace is two sends (197-byte
  headers, 212-byte body). Focused 3x A/B improved warm-50 RPS 32.6% and
  mixed-assets 16.2%; lifetime/trial cgroup stayed at or below baseline and
  RSS changed +1.3% at worst.

Rejected and absent:

* **P2.R8:** moving queue attachment maintenance out of the accept loop did
  not sustain a gain; reverted.
* **P2.R10:** one eager dispatch context reduced RPS about 20%; reverted.

All prior retained P2.R1/R2/R4 changes remain: distinct cgroup-v2 lifetime
and sampled trial peaks, policy-derived passthrough facilities, and exact-count
site/route storage.

Each current cell is `RPS / lifetime-cgroup / trial-cgroup / RSS` as a percent
of its plain-server comparison. A pass requires RPS >=98% and each memory
ratio <=102%.

| Cell | Standalone vs NGINX | Standalone vs Apache |
| --- | --- | --- |
| warm 1 | 68.5 / 48.2 / 53.1 / 63.5 — fail | 264.4 / 19.4 / 17.8 / 57.4 — pass |
| warm 10 | 92.4 / 50.3 / 55.0 / 64.3 — fail | 62.8 / 18.9 / 18.8 / 54.8 — fail |
| warm 50 | 72.0 / 50.3 / 48.5 / 63.8 — fail | 64.0 / 17.8 / 14.9 / 50.2 — fail |
| warm 100 | 49.1 / 50.3 / 50.1 / 63.8 — fail | 84.8 / 16.3 / 14.7 / 45.2 — fail |
| warm 500 | 73.8 / 52.8 / 54.1 / 63.8 — fail | 83.8 / 11.9 / 11.3 / 30.3 — fail |
| warm 1000 | 125.8 / 55.7 / 58.1 / 63.3 — pass | 112.2 / 10.7 / 10.3 / 25.4 — pass |
| JavaScript 10 | 74.4 / 65.0 / 53.7 / 63.8 — fail | 72.9 / 12.1 / 8.6 / 25.4 — fail |
| mixed assets 1000 | 112.8 / 71.9 / 67.5 / 63.9 — pass | 116.4 / 13.2 / 12.3 / 25.4 — pass |
| cache storm 1000 | 102.6 / 67.6 / 66.8 / 63.8 — pass | 122.2 / 13.3 / 11.8 / 25.6 — pass |
| cache thrash 1000 | 111.8 / 81.3 / 63.6 / 63.4 — pass | 122.5 / 15.6 / 11.1 / 25.8 — pass |
| soak 1000 | 52.6 / 85.7 / 75.3 / 61.0 — fail | 68.6 / 18.6 / 14.8 / 25.2 — fail |

**Current comparison-gate status: fail on RPS only.** Memory passes all 22 comparison
cells. Standalone now passes **4/11** against NGINX and **5/11** against
Apache; it previously passed 2/11 and 3/11 respectively in section 22.
Remaining RPS ranges are 49.1–125.8% versus NGINX and 62.8–264.4% versus
Apache.

The previous complete bundle
`/home/debian/laghu-target1-99.NJZAaI/bundle/results.json` matches lane,
production rail, corpus digest, request headers, load matrix, and 99-trial
shape. It is directional only because source/config/image digests differ and
host-state variance is material: current/prior median-RPS ratios span
78.5–147.6% for standalone cells, 71.3–196.3% for NGINX, and 84.0–126.3% for
Apache. No cross-bundle comparison-gate conclusion is inferred.

**Target-1 scope status:** this result authorizes no additional Target-1 profile or architecture cycle.
Root reopening, queue-attachment movement, eager dispatch, allocator, sendfile,
and broad transport rewrites remain out of scope unless they appear as an
explicit audit suggestion and receive user approval.

## 24. Rejected listener accept-batching cycle and remaining Target-1 work

The native listener probe is retained at
`/home/debian/laghu-p2r12-accept-probe.20260825T134303Z/evidence` and its
focused A/B evidence at
`/home/debian/laghu-p2r12-accept-probe.20260825T134303Z/ab-evidence`
(`summary.json` SHA-256
`9dc39c6b40db77fd6ad6955698f0f5ab41dcc915396098781b0aa6c82c11710f`).
It showed listener Recv-Q maxima of 10, 41, and 95 for warm 10/50/100;
soak-1000 reached 808 and was nonzero in 372 of 464 samples. During soak,
132,498 `POLLIN` events, `accept()` calls, and accepted sockets were exactly
equal; EAGAIN and queue rejections were zero; aggregate `accept()` time was
3.985 seconds.

The candidate made the listener nonblocking and drained at most 32 accept
attempts per readiness event. It retried EINTR, stopped on EAGAIN/other accept
errors, retained immediate queue-full 503 handling, and returned to reload,
shutdown, and maintenance between batches. It was rejected: native AMD64
production-scaling 3x interleaved A/B had 12/12 zero-error trials, but warm-50
median RPS was 4,175.815 versus 4,328.161 (96.48%), with sampled trial cgroup
9.20% higher. Soak-1000 median RPS improved to 2,958.286 versus 2,641.469
(111.99%), but lifetime cgroup was 12.49% higher. RSS was effectively flat
(+0.04% warm, -0.29% soak). The long `laghu_proxy_smoke` integration test also
failed on two attempts at differing static/ready assertions, so it could not
provide a clean retain gate. The candidate was reverted; no listener probe or
batch code remains.

Current Target 1 remains RPS-only: all 22 memory gates pass. NGINX still
fails warm 1/10/50/100/500, JavaScript 10, and soak 1000; Apache still fails
warm 10/50/100/500, JavaScript 10, and soak 1000. Narrow mutex evidence is
exhausted (4–5 futex calls per 1,000 requests and microsecond aggregate
wait/hold); P2.R8 queue-attachment movement, P2.R10 eager dispatch, and this
accept batch were rejected. P2.R3 sparse transform-queue initialization stays
deferred because true Target-1 passthrough does not create or touch that
queue.

No additional Target-1 profile or architecture cycle is authorized by this
disposition. Target 2, Target 3, Target 4, and the separate security inventory
remain deferred.

## 25. P2R13 direct passthrough/sendfile re-test: rejected and reverted

The fresh native profile found that Target-1 `rewrite_level: passthrough` still
entered `static_send_transformed`; the first sendfile branch was therefore
unreachable.  Exact current-control tracing for 100 KiB CSS showed one 202-byte
header `sendto`, then buffered `sendto` calls of 65,536 and 36,864 bytes.  The
revised candidate routed only resolved passthrough, identity-coded static
responses around the transform body copy, while preserving policy headers,
conditional/range/HEAD behavior, configured headers, secure root traversal,
and gzip's transformed path.  Its trace showed the same 202-byte header block
and one `sendfile(102400)`; both response digests matched the corpus exactly.

Targeted `laghu_proxy_test` and `laghu_proxy_smoke` passed before rejection.
The latter covered passthrough identity, weak ETag 304, HEAD, range, and gzip
fallback.  No probe code was retained.  The final candidate was reverted after
the user decision.

The native AMD64 production-scaling A/B schedule was baseline, candidate,
candidate, baseline, baseline, candidate for each cell.  All 18 trials had
zero HTTP/check errors.  Values are `RPS | lifetime cgroup | trial cgroup | RSS`
in bytes; the raw source is
`/home/debian/laghu-p2r13-runtime-profile.20260825T142424Z/evidence/ab-evidence/summary.json`
(SHA-256 `262a82fb5a9b5ff1fdc0c6fc6814b90a5f89b366bec4b09d301ec4cf6f8b284c`).

| Cell | Version/order | RPS | Lifetime cgroup | Trial cgroup | RSS |
| --- | --- | ---: | ---: | ---: | ---: |
| warm-100 | baseline/1 | 4032.119 | 10813440 | 9424896 | 31834112 |
| warm-100 | candidate/2 | 4485.899 | 10186752 | 9318400 | 31428608 |
| warm-100 | candidate/3 | 3746.044 | 10452992 | 9543680 | 30994432 |
| warm-100 | baseline/4 | 3838.419 | 10698752 | 9052160 | 31559680 |
| warm-100 | baseline/5 | 3677.971 | 10866688 | 9261056 | 31682560 |
| warm-100 | candidate/6 | 4610.202 | 10678272 | 9031680 | 30601216 |
| soak-1000 | baseline/1 | 3258.603 | 29708288 | 28774400 | 30875648 |
| soak-1000 | candidate/2 | 3097.496 | 36896768 | 35954688 | 31064064 |
| soak-1000 | candidate/3 | 3045.815 | 33062912 | 29827072 | 31174656 |
| soak-1000 | baseline/4 | 2912.629 | 34971648 | 30158848 | 31707136 |
| soak-1000 | baseline/5 | 2922.647 | 33202176 | 30089216 | 31461376 |
| soak-1000 | candidate/6 | 3120.067 | 35667968 | 27299840 | 30806016 |
| CSS-100KiB | baseline/1 | 2215.018 | 12906496 | 12234752 | 31313920 |
| CSS-100KiB | candidate/2 | 2799.363 | 12460032 | 9990144 | 31166464 |
| CSS-100KiB | candidate/3 | 2771.491 | 10997760 | 8859648 | 31129600 |
| CSS-100KiB | baseline/4 | 2426.652 | 12517376 | 10625024 | 32079872 |
| CSS-100KiB | baseline/5 | 1476.890 | 12599296 | 10870784 | 31502336 |
| CSS-100KiB | candidate/6 | 2143.877 | 10616832 | 8892416 | 31350784 |

Median candidate/control ratios were warm-100: RPS 116.87%, lifetime/trial/RSS
96.67%/100.62%/97.83%; soak-1000: 105.98%,
107.43%/99.13%/98.74%; CSS-100KiB: 125.12%,
87.29%/81.80%/98.93%.  Soak lifetime cgroup exceeded the strict 102% cap even
though RPS, trial memory, and RSS passed.  The candidate is rejected, not a
partial retention.

This is a meaningful re-test of audit-1 P7: the earlier candidate improved
plaintext RPS by roughly 25–36% but had 100 KiB cgroup +14.9% with an unclear
cache/buffer cause.  P2R13 used the retained v2 lifetime/trial split and a
direct policy path; it improved CSS memory, but the soak lifetime failure still
disqualifies it.

Target-1 narrow suggestions are now exhausted: retained P1 dead-drained
broadcast removal, P2.R1/R2/R4, P2.R9, and P2.R11 remain; R8, R10,
accept-batching, and P2R13 are rejected; R3 cannot improve true passthrough.
This disposition does not authorize a new connection-lifecycle, worker/queue,
or other architecture slice. Targets 2–4 and security remain deferred.

## Audit scope control

This audit is a bounded suggestion worklist toward the north-star performance threshold. Completing or disposing of a suggestion does not authorize new profile-driven or architectural work; only an explicit audit suggestion and the user approval rules can do that. This document records no broader authorization.

## Approved reload generation reclamation

**Status: accepted.** Native AMD64 exercised eight successful reloads. The
previous retained-snapshot behavior grew RSS by 3.26 MB (8.02%) and cgroup
memory by 4.09 MB (13.10%); accepted behavior grew RSS by 1.33 MB (3.27%) and
cgroup memory by 1.75 MB (5.67%). Preopened document-root descriptors stayed
at 2 rather than growing from 2 to 18.

The implementation uses writer-preferred generation quiescence: requests
borrow and release their immutable configuration generation; reload blocks new
borrowers, waits for active borrowers, swaps configuration/root descriptors,
then frees old generation. At most one heap reload snapshot remains and the
initial caller-owned options are not freed.

Validated: all eight reloads served new routes; concurrent reload/request,
invalid, incompatible, stale, permission, and symlink handoffs retained the
last valid generation; proxy tests, standalone smoke, and ASan/UBSan passed.

## Final numbers and profiling (2026-09-02) — CLOSED

### Closure inventory

| Item | Final disposition | Evidence / final state |
| --- | --- | --- |
| P0 rail metrics, feature-derived lifecycle, compact config, reload reclamation | Accepted | Retained P2.R1/R2/R4 and accepted reload-generation reclamation. |
| P1 dead drained-broadcast and P2.R9 preopened roots | Accepted | Retained native AMD64 evidence in sections 22–23. |
| P2.R8, P2.R10, accept batching, and P2R13 sendfile | Rejected | Measured candidates regressed a locked RPS or memory gate; all reverted. |
| P2.R3 sparse transform queue | Inapplicable | True Target-1 passthrough does not create/touch it. |
| P2.R11 profiling / flamegraph | Blocked | `perf` is absent and `perf_event_paranoid=3`; no flamegraph claim is made. Native `strace` is retained as the permitted alternative. |
| S1–S6 remote/local security findings | Accepted | Slowloris, direct-TLS body handling, request grammar, credential storage, purge-token TOCTOU, and root Chrome sandbox behavior have their accepted dispositions above. |
| Cargo, OCI CVE, immutable supply chain, production C hardening | Accepted | Current Cargo advisory scan is clean; OCI/supply-chain/hardening evidence is retained in sections 16–19. |
| Final stable libyaml 0.2.6 upgrade | Blocked upstream | Official releases still list `v0.2.6-rc.1` as pre-release and `v0.2.5` as latest; Laghu retains 0.2.5 plus the accepted 1,000-depth backport. [GitHub releases](https://github.com/yaml/libyaml/releases) |
| Section 20 HTTP chunk-parser corpus / regression | Accepted | Five-file local corpus remains covered by `laghu_proxy_chunked_regression` (native Linux/AMD64 Clang CTest: 1/1 passed, 0.05 s). `laghu_proxy_chunked_fuzz` compiled with fuzzer/ASan/UBSan and completed 20,000 seeded local iterations: exit 0, 1.503 s, no sanitizer finding. |

### Hardened Release Target-1 directional diagnostic

`linux-fast-build` ran exactly one fresh 15-second, 100-VU `/index.html` trial
per Target-1 comparator using the existing `production-scaling` controls: four
standalone workers, four-CPU quotas, `runtime.access_log: off`, production
passthrough, `Host: alpha.bench.test`, and identity encoding. All requests
were HTTP 200/check-clean.

| Comparator | Requests | RPS | Errors |
| --- | ---: | ---: | ---: |
| Standalone no-optimization | 85,029 | 5,664.60 | 0 |
| Plain NGINX | 157,541 | 10,485.53 | 0 |
| Plain Apache | 107,189 | 7,137.14 | 0 |

Standalone-only lifetime cgroup peak was 9,191,424 bytes before and 11,051,008
bytes after its trial; RSS snapshots were 30,212,096 and 30,629,888 bytes.
The actual release binary is a DYN PIE with GNU_RELRO, non-executable stack,
`BIND_NOW`, and `FLAGS_1 NOW PIE`. Raw k6 JSON, exact configuration hashes,
ELF report, image identity, cgroup/RSS snapshots, and manifest are retained at
`/home/debian/laghu-audit-closure-20260902T205831Z/evidence/release-directional-r3/`
(`summary.json` and `manifest.sha256`); ELF evidence is beside it as
`../release-directional-laghu.readelf`.

This is **directional only**: one short run per comparator, not the 99-trial /
33-median rail. It creates no Target-1 gate result and does not change the
section-23 native verdict.

### R11 profiling disposition

`perf`/flamegraph remains **BLOCKED** on `linux-fast-build`: `perf` is not
installed and `/proc/sys/kernel/perf_event_paranoid` is `3`. The retained
native alternative is a 15-second / 100-VU standalone `strace` run with 36,248
HTTP 200 responses at 2,412.754651 RPS: 14,659 `accept`, 29,155 `sendto`
(1.99/request), and 354 `futex` calls. Raw evidence is
`/home/debian/laghu-audit-closure-20260902T205831Z/evidence/r11-k6.stdout`
and `r11-strace.log`; the earlier focused raw trace remains at
`/home/debian/laghu-p2r11-profile.70Mk42/evidence/warm-50.strace.txt`.
It is syscall evidence, not a replacement for sampled CPU profiling.

No Pass-2 item lacks an accepted, rejected, inapplicable, or blocked
disposition. **Audit-2 is CLOSED.**
