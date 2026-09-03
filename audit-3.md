# Laghu audit — Pass 3

**Audit date:** 2026-09-02  
**Repository:** `Code-Vedas/laghu`  
**Audited ref:** `main`  
**Immutable audited commit:** `99c0e1fe240e8f0fca2285fa5e0a8356a4c802e6`  
**Audit mode:** full-repository static source review, prior-decision reconciliation, and read-only GitHub governance/automation review  
**Themes:** throughput/RPS, memory, availability, application security, worker isolation, dependency assurance, and repository controls  
**GitHub access:** strictly read-only; this audit created no branch, commit, issue, pull request, setting change, workflow run, or other GitHub mutation

## 0. Status

The **review portion of audit 3 is complete**. Remediation is intentionally **open** until every item below is explicitly accepted, rejected, ignored, or deferred and the accepted items satisfy their acceptance gates.

This pass treats `audit-1.md` and `audit-2.md` as binding decision history. It does not relabel previously rejected experiments as new findings merely because the corresponding code paths still exist.

### Executive conclusion

Audit 2 substantially improved Laghu's fixed-memory footprint, reload behavior, request parsing, static-root handling, credential storage, worker isolation, and supply-chain controls. Those decisions remain valid.

The most valuable new work is now outside the already-closed static Target-1 experiment loop:

1. The existing **upstream origin pool is not delivering its intended reuse** on the common non-chunked bypass path.
2. Origin acquisition **destructively removes pooled connections for other authorities**, defeating reuse under multi-origin or failover traffic.
3. The shared C queue clears an entire configured payload slot on every dequeue, causing avoidable memory bandwidth and page dirtying for small jobs in large slots.
4. Basic-auth scrypt work occurs **before** the configured request rate limiter and leaks username existence through a large timing difference.
5. The optional Chrome analyzer executes captured HTML without a real no-network boundary.
6. Main proxy response reads and writes have idle socket timeouts but lack total monotonic deadlines, allowing slow-progress peers to pin blocking workers.
7. Two asynchronous HTTP workers wait for connection close instead of completing from HTTP framing; one also accepts ambiguous or incomplete chunk framing.
8. The repository defines strong CI/security workflows, but `main` is currently unprotected, the audited head has no push-triggered CI/CodeQL run, and the CodeQL workflow's last observed run failed before analysis because a required Brotli build dependency was absent.

These are actionable defects and assurance gaps. They do **not** justify reopening sendfile, accept batching, eager dispatch, regex precompilation, allocator replacement, LTO/PGO, or a broad downstream reactor rewrite.

---

## 1. Primary findings

| ID | Priority | Severity | Theme | Finding | Confidence | Recommended disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `A3-RPS-01` | P0 | High | RPS / latency | Common non-chunked bypass responses always release the origin connection as non-reusable | High | **Accept** |
| `A3-RPS-02` | P0 | High | RPS / latency | Acquiring one authority drains pooled connections belonging to other authorities | High | **Accept** |
| `A3-SEC-01` | P0 | High | Availability / auth | Basic-auth scrypt runs before the configured rate limiter | High | **Accept** |
| `A3-SEC-03` | P0 | High, conditional | Worker isolation / SSRF | Chrome analyzer's DNS flag is not a no-network sandbox and explicitly leaves localhost reachable | High | **Accept when Chrome analysis is enabled** |
| `A3-MEM-01` | P1 | High performance impact | Memory bandwidth / worker RPS | C queue zeroes the full payload capacity after every dequeue | High | **Accept** |
| `A3-SEC-04` | P1 | High | Availability | Main proxy has no total deadline for upstream response reads or downstream response writes | High | **Accept** |
| `A3-GOV-01` | P1 | High assurance impact | Repository integrity | `main` is unprotected and the audited head has no enforced CI/CodeQL gate | High | **Accept** |
| `A3-GOV-02` | P1 | Medium assurance impact | Static analysis | CodeQL build omits required Brotli development files and the last observed analysis stopped before CodeQL ran | High | **Accept** |
| `A3-SEC-02` | P2 | Medium | Authentication confidentiality | Basic auth leaks whether a username exists through KDF timing | High | **Accept** |
| `A3-SEC-05` | P2 | Medium | Worker availability / HTTP integrity | Resource-fetch and HTML-refresh wait for EOF; resource-fetch also accepts weak chunk framing | High | **Accept** |
| `A3-SEC-06` | P2 | Medium, conditional | TLS tenant isolation | Downstream SNI selection and HTTP Host routing are independent | High | **Accept for multi-tenant TLS boundaries** |
| `A3-SEC-07` | P2 | Medium, conditional | Local filesystem hardening | POSIX runtime files are opened through pathnames without anti-symlink and ownership/type validation | High | **Accept where runtime directories cross trust boundaries** |

### Defense-in-depth item

| ID | Priority | Severity | Theme | Item | Confidence | Recommended disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `A3-DI-01` | Measurement-gated | Medium if demonstrated | Worker resilience | Long-lived JavaScript optimizer has no per-job CPU/memory watchdog | Medium | **Measure first; accept only with evidence** |

### Count

* **12 primary findings**
* **7 High**
* **5 Medium**, including deployment-conditional items
* **1 measurement-gated defense-in-depth candidate**
* **0 previously rejected audit experiments reopened**

---

# 2. Binding inherited decision ledger

## 2.1 Accepted work from audits 1 and 2 that remains retained

The following outcomes are treated as product constraints and should remain intact unless a later regression test proves otherwise:

* benchmark memory semantics distinguish lifetime footprint from per-trial workload behavior;
* feature-derived lifecycle initialization avoids creating transformation facilities in true passthrough mode;
* fixed-capacity configuration was compacted;
* old configuration generations are reclaimed safely after reload;
* static document roots are preopened and retained;
* configured response-header operations are batched;
* downstream request reads have monotonic total deadlines in addition to idle limits;
* request-line, header, `Content-Length`, and `Transfer-Encoding` grammar was tightened;
* scrypt-v1 is the normal Basic-auth storage format;
* legacy SHA-256 credentials remain a deprecated read-only migration format rather than being silently rewritten by request handling;
* purge-token file reading uses inode/metadata validation and no-follow behavior;
* Chrome refuses root execution so the browser sandbox remains enabled;
* YAML nesting is bounded;
* chunk parsing has fuzz/regression coverage;
* release hardening, immutable action pins, SBOM/provenance/signing, Cargo advisory checking, and image scanning remain required;
* Target-1 fixed-memory reductions and reload reclamation remain accepted.

Audit 3 must not regress any of these while fixing the new findings.

## 2.2 Closed and rejected items that audit 3 does not reopen

The following remain closed:

* broad downstream keep-alive/repark or event-loop architecture;
* regex precompilation;
* accept batching;
* eager dispatch;
* moving queue attachment maintenance without new evidence;
* sendfile/direct passthrough;
* jemalloc or another allocator replacement;
* IPO/LTO/PGO;
* speculative compiler tuning;
* broad request-lifecycle rewrites merely to chase the closed Target-1 result.

`A3-RPS-01` and `A3-RPS-02` are **not** a revival of the rejected downstream keep-alive architecture. They correct the already-implemented, configured **upstream origin pool** so it behaves as its current contract implies.

`A3-MEM-01` is **not** the closed audit-2 proposal to move queue attachment work. It removes deterministic full-slot memory writes from the existing queue consumer.

`A3-SEC-04` is **not** a reactor rewrite. It adds bounded completion to the existing blocking I/O model.

## 2.3 Target-1 performance rail remains closed unless explicitly reopened

Audit 2 closed the 99-trial Target-1 cycle after the accepted memory work passed its memory gates while RPS remained below the required cross-server rail. Audit 3 does not manufacture a new static-file conclusion and does not authorize another architecture/profile cycle.

The new RPS findings concern **reverse-proxy origin traffic**, particularly HTTP/TLS connection reuse. They require their own proxy benchmark rail and must not be presented as proof that the closed static Target-1 gate now passes.

---

# 3. Method and limitations

## 3.1 Reviewed surfaces

This pass reviewed the current immutable tree and decision history across:

* standalone server request parsing, route selection, TLS, upstream connection management, proxy streaming/buffering, access controls, lifecycle, reload, static serving, and administration;
* shared runtime queues, mappings, cache/runtime filesystem operations, serialization, and worker contracts;
* image, JavaScript, browser-analysis, resource-fetch, HTML-refresh, asset-upload, and telemetry workers;
* NGINX and Apache integration surfaces through repository-wide searches and inherited audit evidence;
* build configuration, dependency manifests and lockfiles;
* CI, CodeQL, Dependabot, release scanning, signing, SBOM/provenance, and branch enforcement state;
* `audit-1.md` and `audit-2.md`, including accepted, rejected, ignored, inapplicable, blocked, and closed work.

Repository-wide searches also covered dangerous process execution, unsafe string APIs, temporary files, TLS verification disabling, browser sandbox disabling, socket timeout patterns, shared-file creation, and queue consumption.

## 3.2 What this pass did not claim

This audit did **not**:

* modify GitHub or the repository;
* execute a local build, unit suite, sanitizer suite, fuzz corpus, benchmark, Cargo audit, CodeQL database, or container image scan;
* claim a fresh CVE-clean result;
* claim exploitability where a finding is explicitly deployment-conditional;
* replace the retained native benchmark evidence from audits 1 and 2;
* infer that a workflow did not run for a particular hidden administrative reason.

The source and governance findings below are high-confidence because their control flow or repository state is directly observable. Performance magnitude still requires the specified acceptance measurements.

---

# 4. Detailed findings

## A3-RPS-01 — common bypass proxy responses never return the origin socket to the pool

**Priority:** P0  
**Severity:** High performance impact  
**Confidence:** High  
**Status:** Accepted with caveats; functional reuse verified
**Recommended decision:** **Accept**

### Evidence

In `servers/laghu/src/proxy.c`:

1. `origin_reusable` starts as `false`.
2. Laghu sends origin requests as HTTP/1.1 with `Connection: keep-alive`.
3. When transaction preparation chooses `LAGHU_HTTP_ACTION_BYPASS`, a normal non-chunked response is sent through `proxy_stream_body(...)`.
4. On success, that branch jumps directly to `done`.
5. The code that computes `origin_reusable` appears only in the later buffered-body block.
6. `done` calls `proxy_origin_release(..., origin_reusable)`.

Therefore a successful, fully consumed, HTTP/1.1, `Content-Length` bypass response still reaches release with `origin_reusable == false`. The pool closes the connection instead of retaining it.

This includes the ordinary reverse-proxy case that should benefit most from origin keep-alive. Bodyless responses in this branch are also not reused.

### Impact

For plaintext origins, Laghu pays repeated TCP setup and kernel bookkeeping.

For TLS origins, it additionally pays repeated TLS handshakes, certificate verification, key exchange, allocations, and latency. Under concurrency this can:

* reduce proxy RPS;
* increase tail latency;
* increase CPU per request;
* create connection churn and ephemeral-port pressure;
* increase origin accept load;
* hide the benefit of the configured `origin_pool_size`.

This is a correctness defect in the existing pooling implementation, not a speculative micro-optimization.

### Required change

Move framing/reusability determination so both streaming and buffered paths can use it.

A connection may be marked reusable only when all of the following are true:

* the response is HTTP/1.1 or otherwise explicitly persistent;
* neither side's relevant response headers nominate `Connection: close`;
* the response boundary is known through a bodyless status/method, exact `Content-Length`, or a successfully validated terminal chunk;
* the complete framed body was consumed;
* no origin protocol/TLS error occurred;
* no early downstream failure caused Laghu to abandon unread origin bytes;
* shutdown/force-stop is not active.

Until-close bodies remain non-reusable.

If the downstream client disconnects while Laghu still owes origin bytes, either drain the bounded remainder before reuse or close the origin. Do not return a partially consumed socket to the pool.

### Tests

Add an origin fixture that counts accepted TCP connections and requests per connection.

Cover:

* HTTP and HTTPS origins;
* `Content-Length` responses;
* HEAD;
* 204 and 304;
* `Connection: close`;
* truncated bodies;
* origin timeout;
* early downstream disconnect;
* chunked terminal success and malformed chunk failure;
* forced shutdown.

### Acceptance gate

* Repeated sequential bypass requests reuse an origin connection after the first request.
* Under bounded concurrency, accepted origin connections remain close to active worker concurrency rather than request count.
* No socket is reused after truncation, parser failure, timeout, shutdown, or unread remainder.
* Proxy RPS and p95/p99 latency do not regress in plaintext tests and improve or remain neutral in TLS tests.
* Existing Target-1 static rails and memory rails remain unchanged.

### Rollback condition

Revert if any test demonstrates cross-response contamination, reuse after incomplete framing, or a reproducible RPS/memory regression that cannot be corrected locally.

### Decision record

* [X] Accept
* [ ] Reject
* [ ] Ignore
* [ ] Defer
* Notes: User accepted with caveats. The patch pools only fully consumed HTTP/1.1 non-chunked bypass responses with a known `Content-Length` or bodyless boundary; `Connection: close`, truncation, EOF/until-close, downstream RST/failure, unread remainder, and force-stop stay non-reusable. Local CTest passed 51/51; AppleClang ASan/UBSan focused tests passed 2/2 plus proxy smoke; independent review found no actionable findings. Docker arm64 and native Linux AMD64 functional smoke passed when only the reload `memory.current` numeric assertion was disabled. Inherited sequential medians were 4261.48 to 5490.59 RPS, p50 0.213 to 0.138 ms, p95 0.333 to 0.319 ms, and p99 0.364 to 0.390 ms; concurrent RPS was 64.55 to 66.45 and p50 15.931 to 15.691 ms. The fixed concurrent run reused four origins `[64,64,64,65]` versus 240 baseline accepts. Retained limits: plaintext p99 increased 7.14%; TLS functional reuse passed but no TLS numeric benchmark was captured; standard Docker/native smoke stopped before RPS at the reload 2 MiB cgroup limit (66080768 to 69595136 and 66895872 to 69398528 bytes); GNU LSan reported 238200 bytes from config/core allocation paths with no A3-RPS-01 stack; macOS LSan is unsupported.

---

## A3-RPS-02 — acquiring one origin authority drains other authorities from the shared pool

**Priority:** P0  
**Severity:** High performance impact  
**Confidence:** High  
**Status:** Accepted with caveats; authority isolation and EOF handling verified
**Recommended decision:** **Accept**

### Evidence

`proxy_origin_acquire()` in `servers/laghu/src/origin.c` inspects a candidate at the current pool index, immediately replaces that entry with the last pool element, decrements `origin_count`, and disposes the candidate unless it is the requested live match.

Because the loop index starts at zero and removed entries are not preserved, asking for authority B removes pooled authority-A sockets while searching for B.

The same function's liveness probe also has ambiguous EOF handling:

* it polls for readable/hangup/error state;
* it peeks one byte;
* when `recv()` returns zero, the expression consults `errno`, whose value is not defined by a successful EOF return.

A stale `EAGAIN` can therefore make EOF appear alive, followed by a failed attempt to use a closed socket.

### Impact

Alternating origins, route-specific upstreams, multi-site traffic, and failover targets can continuously destroy one another's idle connections. This causes:

* repeated TCP/TLS setup;
* lower RPS and higher tail latency;
* origin accept churn;
* poor behavior precisely when multiple origins are configured;
* misleading pool-size tuning, because capacity exists but entries do not survive unrelated lookups.

### Required change

Search without destructively removing healthy nonmatching entries.

Remove only:

* the selected matching connection;
* expired entries;
* definitively dead entries.

Preserve healthy entries for other authorities.

At minimum, implement a stable scan with swap-removal only for entries being removed. Prefer an authority-partitioned pool or an indexed structure if measurements show scan contention, but do not add complexity before the correctness fix is measured.

Correct the liveness check with explicit branches:

* `recv() > 0`: unexpected unread response data; do not reuse;
* `recv() == 0`: peer closed; dead;
* `recv() < 0 && EAGAIN/EWOULDBLOCK`: no queued data; alive;
* any other error: dead.

If the pool is full at release time, use an explicit eviction policy rather than silently allowing the newest authority to destroy all older authorities during the next acquisition.

### Tests

Use two independent HTTP and TLS fixtures, A and B, each recording accepted connections.

Test:

* A, B, A, B sequentially;
* concurrent A/B traffic;
* one expired A entry plus one live B entry;
* one closed A entry plus one live B entry;
* a pooled socket with unexpected unread bytes;
* full-pool eviction;
* route failover.

### Acceptance gate

* A request for B does not close a healthy pooled A connection.
* Returning to A reuses A's prior connection.
* EOF is never classified as live.
* Pool count and ownership remain correct under sanitizer and thread-stress tests.
* Alternating-origin RPS/latency is neutral or better; no static Target-1 regression.

### Rollback condition

Revert if pool ownership, socket lifecycle, or shutdown tests expose a race that cannot be resolved without broad architecture change. Preserve the explicit liveness correction even if a larger indexing experiment is rolled back.

### Decision record

* [X] Accept
* [ ] Reject
* [ ] Ignore
* [ ] Defer
* Notes: User accepted with caveats. Stable scan now preserves healthy nonmatching authority entries; it removes only selected, expired, or dead entries. EOF is dead, unread raw/TLS plaintext is non-reusable, and a full shared pool deterministically evicts its oldest idle entry. HTTP/TLS A/B, failover, expiry, closed/unread sockets, eviction, and concurrent smoke passed. Local CTest passed 51/51; AppleClang ASan/UBSan focused proxy/chunked/smoke passed 3/3 (macOS LSan unavailable). Alternating-origin sequential medians: 4776.47 to 6635.25 RPS, p50 0.1826 to 0.1261 ms, p95 0.3304 to 0.2302 ms, p99 0.4215 to 0.3400 ms; concurrent: 6562.40 to 8170.63 RPS, p50 1.062 to 0.866 ms, p95 1.839 to 1.456 ms, p99 3.454 to 2.466 ms, and 718 to 15 new origin accepts across three trials. TLS functional reuse passed, but no TLS numeric benchmark. Standard Docker arm64 smoke reached the A/B fixture then failed reload cgroup growth (+2732032 bytes, 2.61 MiB, above 2 MiB); a wrapper disabling only cgroup sampling passed. Native Linux AMD64 focused proxy/chunked tests passed 2/2, but full smoke stopped before the new fixture with `laghu: invalid YAML configuration file`. Shared oldest-idle eviction intentionally has no per-authority reservation; Target-1 remains closed with no new static claim.

---

## A3-MEM-01 — C queue consumption zeroes the entire payload capacity for every job

**Priority:** P1  
**Severity:** High performance impact  
**Confidence:** High  
**Status:** Open  
**Recommended decision:** **Accept**

### Evidence

`laghu_runtime_queue_try_take()` in `libs/laghu-runtime/src/queue.c`:

1. reads and validates the actual payload length;
2. copies only that actual length to the consumer;
3. then clears `state->slot_payload_size` bytes, regardless of actual length;
4. clears the fixed slot header and advances the queue.

The default queue has four slots. The image worker creates its queue with `LAGHU_IMAGE_MAX_INPUT_BYTES`, which is 10 MiB, as the per-slot payload capacity.

A 1 KiB image job can therefore cause a 10 MiB memory write when consumed. The same behavior applies to every C queue according to its configured capacity.

The Rust JavaScript queue does not have the same steady-state behavior; it clears its header and actual payload length. Audit 3 does not incorrectly generalize the C defect to the Rust queue.

### Impact

Full-slot clearing can:

* consume memory bandwidth disproportionate to useful work;
* dirty/fault many pages for a small job;
* increase worker CPU time;
* reduce queue throughput;
* increase cgroup `memory.current` activity and page-reclaim pressure;
* amplify contention because the clear occurs while the shared mapping lock is held.

For a 10 MiB slot, even low job rates can generate substantial avoidable writes.

### Security consideration

The current full clear provides a strong stale-data hygiene property. The fix must retain that property without clearing unused capacity on every valid job.

### Required change

For a valid READY slot:

* clear the validated payload length that was actually present;
* clear the fixed header;
* perform the minimum state transition necessary for EMPTY.

For a corrupt or untrusted slot whose length cannot be safely bounded, use a conservative full clear or reinitialize/quarantine that slot.

Do not rely on a future publisher to erase stale bytes. The dequeue transition should leave all bytes that contained the consumed valid payload cleared before the slot is reusable.

Move large clears outside the global queue lock only if ownership/state sequencing makes it impossible for a publisher to reuse the slot concurrently. A simple length-bounded clear under the current lock is the safer first patch.

### Tests

Add deterministic tests for:

* 0-byte, 1-byte, 1 KiB, 64 KiB, 1 MiB, and maximum-size jobs;
* a large job followed by a small job in the same slot;
* corrupt payload lengths;
* interrupted/failed decode;
* producer/consumer stress;
* direct mapping inspection proving consumed payload bytes are erased;
* the Rust queue contract remaining unchanged.

### Measurement

For a queue with 10 MiB slot capacity, benchmark small jobs and record:

* jobs/second;
* CPU cycles/job;
* bytes written or cache misses if available;
* minor faults;
* `memory.current`;
* lifetime and trial cgroup peaks;
* time spent holding the queue mapping lock.

### Acceptance gate

* In the valid steady state, bytes cleared are bounded by fixed header plus actual payload length.
* A large payload's former bytes are not observable after consumption.
* Corrupt slots fail closed and cannot cause an out-of-bounds clear.
* Small-job queue throughput improves or remains neutral.
* Existing queue compatibility/version tests pass across C and Rust consumers.

### Rollback condition

Revert if stale payload bytes become observable through any supported queue API or cross-process mapping contract.

### Decision record

* [ ] Accept
* [ ] Reject
* [ ] Ignore
* [ ] Defer
* Notes:

---

## A3-SEC-01 — memory-hard Basic-auth verification runs before the configured rate limiter

**Priority:** P0  
**Severity:** High availability impact  
**Confidence:** High  
**Status:** Accepted; validation caveats recorded
**Recommended decision:** **Accept**

### Evidence

`proxy_request_access_allowed()` performs:

1. CIDR deny;
2. CIDR allow;
3. Basic-auth verification;
4. request rate limiting.

For scrypt-v1 credentials, `proxy_basic_password_matches()` invokes OpenSSL scrypt with `N=16384`, `r=8`, `p=1`, and a 32 MiB maximum-memory cap.

The rate limiter is therefore reached only after the expensive password operation has completed. If a route uses Basic auth without a rate rule, there is no request-rate control around this KDF path at all.

An attacker does not need a correct password. A known username with repeated wrong passwords is sufficient to force the memory-hard work.

### Impact

Laghu uses a finite blocking worker pool. Parallel authentication attempts can consume worker CPU and memory before application rate controls run, reducing capacity for unrelated traffic.

TLS does not prevent this because the attacker is the connected client. CIDR allowlisting can reduce exposure where configured, but public Basic-auth routes remain vulnerable.

### Required change

Add an inexpensive **pre-authentication admission control** before password KDF work.

It should include:

* a per-direct-peer token bucket;
* an auth-scope or route identifier;
* a process-wide or queue-wide cap on concurrent/rolling KDF work;
* bounded state with clear eviction semantics;
* observability for pre-auth rejection and KDF saturation.

Keep the existing post-auth/request limiter in its current semantic position. The pre-auth limiter protects the authentication resource; the existing limiter protects the route's normal request policy.

Do not key security controls from untrusted `X-Forwarded-For` unless the trusted-proxy boundary has already been validated. Use the direct peer by default.

### Tests

Add a TLS integration fixture with a configured scrypt user.

Test:

* known username, wrong password flood;
* unknown username flood;
* valid credentials;
* multiple source addresses where feasible;
* one hostile source alongside unrelated public/static requests;
* limiter state eviction;
* reload of auth rules;
* shutdown during active KDF work.

### Acceptance gate

* KDF starts per second and concurrent KDF count cannot exceed configured bounds.
* Rejected pre-auth attempts do not execute scrypt.
* Valid low-rate users continue to authenticate.
* Unrelated route throughput and tail latency remain bounded under an auth flood.
* Metrics distinguish pre-auth rejection, credential failure, post-auth rate rejection, and KDF saturation.
* Secrets and decoded credentials remain cleansed on every path.

### Rollback condition

Do not remove the protection because of a poor default. Adjust defaults or scope keys while retaining a hard global KDF budget.

### Decision record

* [X] Accept
* [ ] Reject
* [ ] Ignore
* [ ] Defer
* Notes: User accepted. Pre-auth admission now applies only to a syntactic `Basic ` candidate before Base64 decoding; absent and non-Basic authorization return `401` without consuming a pre-auth token, while malformed Basic candidates are bounded before decode and execute no scrypt. The control uses a bounded direct-peer plus Basic-auth-scope bucket, a global KDF rate/burst/concurrency budget, no queue lock during scrypt, reload-state reset, and aggregate redacted metrics; existing post-auth route limiting remains in place. Normal local CTest under `umask 0002` passed 52/52; Apple ASan/UBSan passed 51/51; lint, docs, and diff checks passed. Docker arm64 rebuilt as root and ran CTest as `laghu-test`: Chrome root-refusal/normal-startup and YAML nesting passed, while proxy smoke reached the auth p95 check (0.0045 to 0.0061 s, limit 0.2500 s) then stopped only at the reload cgroup rule. Native Linux AMD64 YAML nesting passed and proxy smoke reached auth p95 (0.0208 to 0.0361 s, limit 0.2500 s) before the same cgroup rule. That 2 MiB cgroup threshold is explicitly user-ignored and was not changed. Linux `detect_leaks=1` focused `laghu_proxy_test` passed with no LeakSanitizer diagnostic; full sanitizer smoke is not claimed green because its post-auth rail stopped at the existing ASan RSS assertion (208646144 to 345735168 bytes) after the auth check.

---

## A3-SEC-02 — Basic auth exposes username existence through KDF timing

**Priority:** P2  
**Severity:** Medium  
**Confidence:** High  
**Status:** Open  
**Recommended decision:** **Accept**

### Evidence

`proxy_basic_authorized()` compares the decoded username against configured users.

* An unknown username exits without calling a password KDF.
* A matching scrypt-v1 username performs memory-hard scrypt.
* The resulting timing difference is much larger than a normal constant-time hash comparison.

This allows an attacker to distinguish configured usernames before attempting password guessing.

### Impact

Username enumeration increases the efficiency of credential attacks and provides configuration information. The impact is reduced for routes behind strict CIDR allowlists, but remains relevant for public Basic-auth endpoints.

The legacy SHA-256 reader is not reopened by this finding. Audit 2 intentionally retained it as a deprecated migration input. The issue here is failure-path timing.

### Required change

After the new pre-auth admission control from `A3-SEC-01`:

* perform a dummy scrypt verification using fixed process/configuration material when no username matches;
* keep secret comparison constant-time;
* avoid early exits that expose credential-record class;
* during the legacy migration window, make failed legacy-user checks consume comparable work, or define and document the residual timing exception with an expiration plan.

Dummy salt/hash material must not be a credential and need not be secret, but should be stable enough to avoid per-request setup artifacts.

### Tests

Use repeated local timing samples for:

* unknown username;
* known scrypt username with wrong password;
* known legacy username with wrong password while migration support exists;
* valid username/password.

Network end-to-end tests should supplement, not replace, an instrumented unit/microbenchmark that verifies equivalent KDF execution counts.

### Acceptance gate

* Unknown and known-wrong scrypt attempts execute the same KDF class after pre-auth admission.
* Timing distributions no longer expose a deterministic fast unknown-user branch.
* Valid authentication behavior is unchanged.
* The fix does not permit unlimited dummy-KDF abuse; `A3-SEC-01` must land first or in the same change.

### Rollback condition

Do not ship dummy KDF equalization without pre-auth/global KDF bounds.

### Decision record

* [ ] Accept
* [ ] Reject
* [ ] Ignore
* [ ] Defer
* Notes:

---

## A3-SEC-03 — Chrome analysis executes captured HTML without a real no-network boundary

**Priority:** P0  
**Severity:** High, conditional on the feature being enabled  
**Confidence:** High  
**Status:** Open  
**Recommended decision:** **Accept when Chrome analysis is enabled**

### Evidence

The Chrome worker:

* receives an HTML snapshot through the browser-analysis queue;
* writes the captured HTML to a temporary local file;
* injects its analysis script;
* launches headless Chromium on a `file://` URL;
* leaves normal page JavaScript execution available;
* uses `--host-resolver-rules=MAP * 0.0.0.0, EXCLUDE localhost`.

That resolver rule is not network isolation:

* `localhost` is explicitly exempted;
* IP literals do not require DNS resolution;
* loopback and local service addresses can still be targeted;
* disabling background networking does not prohibit page-initiated networking.

The outer worker timeout bounds duration, and the worker correctly refuses root so Chromium's sandbox remains enabled. Those are valuable controls, but neither prevents network access initiated by captured HTML.

### Threat model

The HTML is derived from an origin response selected for analysis. A compromised or intentionally hostile origin can include script or markup that attempts requests to:

* `localhost`;
* `127.0.0.1`;
* `[::1]`;
* wildcard/listener addresses;
* local service DNS;
* cloud metadata addresses;
* other interfaces reachable from the worker namespace.

Same-origin policy may prevent reading many responses, but it does not reliably prevent blind or state-changing requests. Therefore the primary risk is blind SSRF and access to worker-local services.

File-origin access behavior must also be tested rather than assumed safe.

### Required change

Run each browser-analysis job inside a true isolation boundary:

* a dedicated network namespace, container, or equivalent with no usable egress;
* no loopback service exposure beyond what the isolated browser itself requires;
* no inherited host network;
* a minimal dedicated temporary directory;
* unprivileged UID/GID;
* Chromium sandbox retained;
* no broad host filesystem mount;
* browser/CDP request interception that aborts all network requests as a second control;
* resolver rule without a localhost exemption as defense in depth, not as the primary boundary.

Keep the existing process-group kill and timeout.

If no reliable no-network boundary is available on a supported platform, fail closed or disable the feature on that platform.

### Tests

Start canary listeners and attempt from captured HTML:

* `http://localhost`;
* `http://127.0.0.1`;
* `http://[::1]`;
* `http://0.0.0.0`;
* a local service hostname;
* a private RFC1918 address;
* a link-local metadata address;
* external DNS and literal external IP;
* `file://` paths outside the job directory;
* redirects to restricted addresses;
* WebSocket, fetch, form, image, script, CSS, and navigation requests.

### Acceptance gate

* Canary listeners observe zero connections.
* Packet/network-namespace evidence shows no egress from the job.
* Analysis still produces the expected report for self-contained HTML.
* Chromium never runs as root and never uses `--no-sandbox`.
* Timeout and process-group cleanup remain effective.
* The service fails closed if isolation setup fails.

### Rollback condition

Disable Chrome analysis rather than running it with a best-effort DNS-only restriction.

### Decision record

* [ ] Accept
* [ ] Reject
* [ ] Ignore because Chrome analysis is permanently disabled
* [ ] Defer
* Notes:

---

## A3-SEC-04 — proxy response I/O has idle timeouts but no total monotonic deadline

**Priority:** P1  
**Severity:** High availability impact  
**Confidence:** High  
**Status:** Open  
**Recommended decision:** **Accept**

### Evidence

`proxy_timeout()` configures `SO_RCVTIMEO` and `SO_SNDTIMEO`.

Audit 2 added `proxy_client_recv_until()` for downstream **request reads**, with a monotonic total deadline that is not reset after successful bytes. The source explicitly states that upstream response reads retain their independent existing timeout behavior.

The following response paths loop without an end-to-end deadline:

* origin response-header reads;
* origin body reads/streaming;
* TLS/plain origin sends;
* TLS/plain downstream response writes.

A peer that makes small periodic progress can avoid an idle timeout indefinitely.

### Impact

Because the standalone server uses blocking workers, either side can pin workers:

* a malicious or compromised origin trickles headers/body;
* a slow downstream client accepts occasional response bytes;
* enough concurrent slow-progress connections exhaust the worker pool;
* unrelated requests queue or fail.

This is a bounded-I/O defect, not an argument for an event loop.

### Required change

Add configurable monotonic completion budgets for:

* origin response headers;
* origin response body;
* downstream response writes.

Consider both:

* an absolute maximum duration; and
* a minimum sustained transfer rate after a grace period for bodies above a threshold.

Each loop must recompute remaining time against a fixed deadline. For TLS `WANT_READ`/`WANT_WRITE`, poll only for the remaining bounded interval.

Do not apply one inflexible body duration to all object sizes. Defaults should be size-aware or combine total maximum plus minimum rate.

Propagate distinct failure reasons, for example:

* `origin_header_total_timeout`;
* `origin_body_total_timeout`;
* `client_write_total_timeout`.

Return protocol-appropriate 502/504 only when a response has not already begun; otherwise terminate the connection and log the reason.

### Tests

Add fixtures for:

* one origin-header byte just before every idle timeout;
* one body byte just before every idle timeout;
* downstream client reading one byte periodically;
* TLS `WANT_READ` and `WANT_WRITE`;
* large legitimate slow transfer within the configured rate;
* shutdown while blocked;
* normal high-RPS proxy traffic.

### Acceptance gate

* Slow-progress fixtures terminate within the configured total bound.
* A bounded number of malicious origins/clients cannot permanently consume all workers.
* Normal large transfers within policy complete successfully.
* Failure metrics and logs distinguish idle from total timeout.
* Existing request Slowloris protection remains unchanged.
* No closed architecture experiment is reintroduced.

### Rollback condition

Adjust size/rate defaults if legitimate transfers are harmed; do not remove total bounded completion from public blocking-worker paths.

### Decision record

* [ ] Accept
* [ ] Reject
* [ ] Ignore
* [ ] Defer
* Notes:

---

## A3-SEC-05 — worker HTTP clients wait for EOF, and resource-fetch accepts weak chunk framing

**Priority:** P2  
**Severity:** Medium  
**Confidence:** High  
**Status:** Open  
**Recommended decision:** **Accept**

### Evidence

### Shared EOF behavior

`laghu-resource-fetch` and `laghu-html-refresh` send `Connection: close` and read into a bounded buffer until `SSL_read()` stops. They parse the response only after EOF/error.

This means a complete `Content-Length` body is not considered complete while the server keeps the connection open. A peer can also extend the operation by sending occasional bytes within each per-read timeout.

Requesting `Connection: close` is not a substitute for parsing the response boundary.

### Resource-fetch-specific chunk behavior

The resource-fetch parser:

* recognizes transfer encoding with a case-sensitive substring search for `"chunked"`;
* can accept values containing that substring rather than a strict coding token;
* does not reject `Transfer-Encoding` plus `Content-Length`;
* overwrites repeated `Content-Length` values rather than requiring identical values;
* exits the chunk loop when input ends and still returns the decoded body even if no zero-size terminal chunk was received;
* breaks on a zero-size chunk without strict trailer/final-boundary validation.

`laghu-html-refresh` has a stricter terminal-chunk decoder. Audit 3 does not incorrectly assign the resource-fetch parser defects to it. It does share the wait-for-EOF completion problem.

### Impact

These are asynchronous workers, so the main proxy hot path is insulated. Still, malicious or misbehaving providers/origins can:

* hold worker capacity until timeout;
* slow refresh/fetch progress;
* cause repeated retry pressure;
* feed ambiguous or truncated CSS to downstream processing;
* turn a bounded queue into a starvation point.

TLS verification, provider allowlisting, public-address checks, redirect limits, and size caps reduce exposure and remain retained.

### Required change

Create or reuse a strict incremental HTTP/1 response reader for these workers.

It must:

* enforce the status-line and header grammar;
* cap header count and bytes;
* reject conflicting framing;
* treat `Transfer-Encoding` as a token list and require supported `chunked` to be final;
* reject `Transfer-Encoding` plus `Content-Length`;
* require repeated `Content-Length` values to be identical or reject them;
* finish immediately after exact `Content-Length`;
* require a terminal zero chunk;
* bound and validate trailers;
* reject bytes after the allowed framed message where the connection contract requires it;
* retain both idle and total deadlines;
* cap redirects and reapply destination validation after every redirect.

Do not merely enlarge the response buffer or timeout.

### Tests

Add HTTP/TLS fixtures for:

* complete `Content-Length` body while connection remains open;
* early EOF;
* too-long body;
* duplicate equal and unequal lengths;
* `Transfer-Encoding` plus length;
* `chunked`, `Chunked`, `xchunked`, and multiple codings;
* missing zero chunk;
* bad chunk extension;
* bad CRLF;
* valid bounded trailers;
* excessive trailers;
* slow trickle;
* redirect from public to private/loopback target.

### Acceptance gate

* A framed response completes without waiting for EOF.
* Truncated and ambiguous messages fail closed.
* Resource-fetch never accepts a chunk stream without a valid terminal zero chunk.
* Existing destination/TLS validation remains intact.
* Worker throughput is neutral or better under normal providers.
* Retry behavior remains bounded.

### Rollback condition

If sharing one parser creates excessive coupling, keep separate adapters over one tested framing state machine rather than reverting to EOF semantics.

### Decision record

* [ ] Accept
* [ ] Reject
* [ ] Ignore
* [ ] Defer
* Notes:

---

## A3-SEC-06 — downstream TLS SNI and HTTP Host select sites independently

**Priority:** P2  
**Severity:** Medium, conditional  
**Confidence:** High  
**Status:** Open  
**Recommended decision:** **Accept where SNI is a tenant boundary**

### Evidence

The downstream SNI callback:

* returns success when SNI is absent;
* returns success when SNI does not match a configured site;
* switches certificate context only on an exact configured host;
* does not retain the selected SNI identity for later request validation.

HTTP site selection independently reads `Host` and matches a configured hostname followed by end-of-string or any colon suffix.

There is no check that:

* SNI is known when a multi-site TLS boundary requires it;
* the HTTP Host corresponds to the SNI-selected site;
* a colon suffix is a syntactically valid numeric port.

### Impact

Some deployments intentionally permit shared certificates, domain fronting, or Host-independent routing. For those, strict equality would be a compatibility change.

Where each configured site represents a tenant/security boundary, however, a client can complete TLS with missing, unknown, default, or different SNI and then route through another configured Host. That can weaken assumptions around certificate/site/tenant coupling and logging.

### Required change

Add an explicit policy such as `strict_sni_host`.

When enabled:

* reject missing or unknown SNI before HTTP dispatch;
* normalize SNI and Host as authorities;
* validate optional Host port syntax and range;
* require Host hostname to equal the authenticated SNI hostname;
* preserve a deliberate exception only for documented wildcard/shared-certificate policy;
* log mismatch without reflecting secrets.

Choose the default based on Laghu's documented multi-site contract. If compatibility requires off by default, emit a startup warning when multiple site-specific TLS contexts exist without strict binding.

### Tests

Cover:

* matching SNI/Host;
* missing SNI;
* unknown SNI;
* SNI A with Host B;
* uppercase and trailing-dot normalization policy;
* valid and invalid ports;
* IPv6 authorities if supported;
* wildcard/shared certificate policy;
* plaintext HTTP, where SNI does not exist.

### Acceptance gate

* Strict mode rejects every mismatch before tenant-specific policy/upstream selection.
* Compatibility mode remains explicit and documented.
* Access logs retain enough normalized identity to diagnose mismatches.
* Existing single-site deployments do not regress.

### Rollback condition

Do not enforce globally if the documented product contract supports domain fronting. Retain the strict opt-in and warnings for tenant deployments.

### Decision record

* [ ] Accept
* [ ] Reject
* [ ] Ignore because SNI is not a security boundary
* [ ] Defer
* Notes:

---

## A3-SEC-07 — POSIX runtime files rely on trusted directories but do not enforce that trust

**Priority:** P2  
**Severity:** Medium, conditional  
**Confidence:** High  
**Status:** Open  
**Recommended decision:** **Accept where runtime directories cross trust boundaries**

### Evidence

In `libs/laghu-runtime/src/runtime_posix.c`:

* atomic writes use a predictable `<path>.tmp.<pid>` name;
* the temporary file is opened with create/truncate but without exclusive-create or no-follow flags;
* shared mappings are opened by pathname with create/read-write;
* existing mapping opens do not use no-follow;
* mapping opens validate size but not regular-file type, owner, mode, or link count;
* atomic rename does not fsync the parent directory.

Packaged directories commonly use restrictive modes, which reduces risk when every process with directory write access is equally trusted. The runtime code itself does not prove or enforce that condition.

### Impact

If a less-trusted local principal, compromised worker, container peer, or misconfigured group can write the runtime directory, it may race predictable paths or substitute links/files. Consequences can include:

* redirecting atomic writes;
* mapping an unintended file;
* corrupting queue/cache state;
* local denial of service;
* modifying a file writable by the Laghu service account.

This is a local/deployment-boundary issue, not a remote vulnerability under correctly owned non-writable directories.

### Required change

Use directory-fd-relative operations and validate opened objects:

* open the trusted parent directory once with no-follow/directory flags;
* use `openat()`/equivalent;
* create temporary files with randomized names and `O_CREAT|O_EXCL`;
* apply `O_NOFOLLOW` and `O_CLOEXEC` where supported;
* `fstat()` every mapping/file and require regular file, expected owner, acceptable mode, expected size, and appropriate link count;
* use `fchmod()` after creation rather than relying on process umask;
* fsync the file and parent directory around durable rename;
* fail closed on unsupported security semantics where the directory is configured as shared across trust domains.

Retain platform-specific equivalents for macOS and other supported systems.

### Tests

In an isolated temporary directory, attempt:

* precreated symbolic link at the temporary path;
* link replacement between validation and open;
* FIFO/device/directory instead of regular file;
* wrong owner/mode where test privileges permit;
* hard-link substitution;
* concurrent writers;
* crash after fsync but before/after rename;
* valid normal queue/cache operation.

### Acceptance gate

* Symlink and non-regular-file substitutions fail closed.
* Atomic writes never truncate a preexisting attacker-controlled temporary path.
* Directory durability behavior is documented and tested.
* Correctly owned installations continue to work on all supported platforms.
* No path-hardening regression is introduced into the already-accepted static-root or purge-token code.

### Rollback condition

If portability requires platform-specific implementations, keep the hardened POSIX path and implement an explicit documented fallback rather than silently dropping checks.

### Decision record

* [ ] Accept
* [ ] Reject
* [ ] Ignore because all runtime directories are guaranteed same-trust and non-writable
* [ ] Defer
* Notes:

---

## A3-GOV-01 — `main` is unprotected and the audited head has no enforced CI/security result

**Priority:** P1  
**Severity:** High assurance impact  
**Confidence:** High  
**Status:** Open  
**Recommended decision:** **Accept**

### Evidence

At the audited commit:

* GitHub reports `main` as unprotected;
* required status-check enforcement is off;
* the head has no push-triggered workflow run;
* its visible checks are Dependabot checks rather than the repository's CI or CodeQL jobs.

The repository contains substantial workflow definitions, including matrix builds, tests, lint, release contracts, Cargo audit, sanitizers/fuzzing, CodeQL, release image scanning, SBOM, signing, and provenance. Definitions alone do not provide a merge gate when they do not run or are not required.

This audit does not infer the administrative reason for the missing head run. It records the observable assurance state.

### Impact

A direct push, force push, accidental merge, permission misuse, or workflow-trigger failure can place code on `main` without the controls that the repository appears to rely on.

For a C/Rust proxy and native-module project, unenforced sanitizer, build, parser, and security analysis is a material integrity risk.

### Required change

First, diagnose why the audited head has no CI/CodeQL push run:

* Actions/repository policy;
* workflow state;
* event/actor restrictions;
* organization policy;
* billing/runner availability;
* workflow parsing;
* path/ref behavior.

Then protect `main` with a branch protection rule or repository ruleset that, as supported by the repository's collaboration model:

* requires pull requests;
* requires the intended CI and security checks;
* requires branches to be up to date or uses a merge queue;
* blocks force pushes and branch deletion;
* restricts bypass permissions;
* dismisses stale approvals;
* requires conversation resolution;
* optionally requires signed commits/tags if that is the project contract.

For a sole-maintainer project, one approval may be impractical. Required checks, no-force-push, no-delete, and restricted bypass remain valuable even if review count is zero.

Do not require a check until it runs reliably on pull requests and `main`; otherwise protection can deadlock maintenance.

### Acceptance gate

* A fresh pull request starts all intended required checks.
* A deliberately failing test blocks merge.
* A successful PR shows the required green checks and can merge.
* A direct non-bypass push is rejected according to policy.
* `main` reports protection/ruleset enforcement.
* Force push and deletion are disabled or narrowly controlled.
* Emergency bypass use is auditable.
* README/contributor/release documentation names the required checks.

### Rollback condition

If a flaky check blocks all work, temporarily remove only that check from required status while fixing it; retain branch protection and the reliable checks.

### Decision record

* [ ] Accept
* [X] Reject
* [ ] Ignore
* [ ] Defer
* Notes: Repository is private and no protection needed just yet

---

## A3-GOV-02 — CodeQL cannot reach analysis because its build environment omits Brotli development files

**Priority:** P1  
**Severity:** Medium assurance impact  
**Confidence:** High  
**Status:** Accepted implementation; hosted verification pending  
**Recommended decision:** **Accept**

### Evidence

The current CodeQL workflow installs:

* build-essential;
* CMake;
* PCRE2 development files;
* OpenSSL development files;
* zlib development files.

The shared runtime CMake configuration requires `libbrotlienc` through pkg-config.

The last observed CodeQL job failed in `scripts/run-build-all` during CMake configuration because `libbrotlienc` was not found. The CodeQL analyze step was skipped.

The current CodeQL workflow still lacks the corresponding Brotli development package, so the same dependency mismatch remains in the workflow definition.

### Impact

CodeQL's presence can create false confidence while no database is finalized and no queries are run. New C/C++ data-flow or memory-safety findings will not be uploaded from that workflow until the build succeeds.

### Required change

* Install the required Brotli development package in CodeQL's Linux setup.
* Run the exact current tree through CodeQL and confirm the analyze/upload step completes.
* Keep CodeQL's dependency setup aligned with the canonical build dependency list instead of maintaining an easily stale partial list.
* Add a release/build contract or shared setup script so CI and CodeQL cannot silently diverge on required native packages.
* Make the successful CodeQL check required only after it is reliable.

Consider including Rust CodeQL only if the supported analyzer and project value justify it; the current confirmed defect is the C/C++ build, not missing speculative coverage.

### Acceptance gate

* CodeQL builds the current tree.
* The analyze step runs and uploads results.
* The workflow is green on both a pull request and `main`.
* A controlled test query/sample alert proves result publication.
* Dependency setup is shared or contract-tested to prevent another missing-package drift.

### Rollback condition

Do not disable CodeQL to obtain a green branch. If build tracing remains problematic, use a supported alternative build mode while preserving meaningful C/C++ coverage.

### Decision record

* [X] Accept
* [ ] Reject
* [ ] Ignore
* [ ] Defer
* Notes: Accepted patch: shared Linux native-dependency setup now serves CodeQL, CI, and release; it includes `libbrotli-dev` and has a supply-chain contract. Local lint/YAML/build/51-test CTest, clean Ubuntu Docker Brotli resolution, and isolated Linux AMD64 `scripts/run-build-all` plus NGINX module linking `libbrotlienc.so.1` passed. A bounds-checked test-path join also removes GCC's post-Brotli `-Werror=format-truncation` blocker. Hosted current-tree CodeQL build, PR and `main` green runs, analyze/upload, and controlled alert publication remain pending; this is not accepted and verified.

---

# 5. Measurement-gated defense-in-depth

## A3-DI-01 — long-lived JavaScript optimizer lacks a per-job resource watchdog

**Priority:** Measurement-gated  
**Severity:** Medium if pathological input is demonstrated  
**Confidence:** Medium  
**Status:** Candidate, not a primary confirmed defect  
**Recommended decision:** **Measure before accepting**

### Observation

The Rust JavaScript worker parses and transforms untrusted, bounded JavaScript in a long-lived process. The queue payload is size-capped, but source-byte limits do not guarantee bounded parser/transform CPU, AST size, recursion behavior, or allocator growth for every syntactically adversarial input.

Unlike the isolated libvips/ffmpeg path, the worker does not place each transform in a supervised child with a per-job CPU/memory watchdog.

A panic aborts the worker. Because the queue item has already been taken, a crash can also lose that job unless a higher-level replay mechanism exists.

### Why this is not yet a primary finding

No pathological sample, uncontrolled growth, or crash was demonstrated in this read-only pass. SWC and Rust provide substantial memory-safety value, and async failure does not directly corrupt the proxy response path.

### Measurement request

Build a hostile corpus within the existing maximum source size:

* extremely deep nesting;
* very large expression/statement counts;
* pathological comments/templates/regex literals;
* repeated scopes and identifiers;
* malformed near-valid inputs;
* source-map-heavy cases;
* known upstream parser regression samples.

Record per job:

* wall and CPU time;
* peak RSS/cgroup memory;
* allocations if available;
* stack behavior;
* worker survival;
* queue/job outcome.

### Accept only if

A realistic bounded input causes a material timeout, memory spike, stack failure, abort, or prolonged queue starvation.

Then choose the narrowest mitigation:

* parser/AST/depth limits;
* supervised child process;
* cgroup/rlimit limits;
* per-job timeout;
* poison-job quarantine and observable retry/drop semantics.

### Decision record

* [ ] Measure
* [ ] Accept now
* [ ] Reject
* [ ] Ignore
* Notes:

---

# 6. Implementation order

## Phase 0 — restore trustworthy gates

1. `A3-GOV-02`: repair CodeQL's build environment and obtain a successful analysis.
2. Diagnose missing CI/CodeQL execution at the audited head.
3. `A3-GOV-01`: protect `main` and require the reliable checks.
4. Capture current Cargo audit, CodeQL, sanitizer/fuzz, and release-image scan outputs.

Reason: large protocol and lifecycle fixes should not proceed while the intended automated safety net is absent.

## Phase 1 — highest-value proxy and auth corrections

1. `A3-SEC-01`: pre-auth/global KDF budget.
2. `A3-SEC-02`: dummy-KDF timing equalization.
3. `A3-RPS-01`: mark successfully consumed streaming responses reusable.
4. `A3-RPS-02`: preserve unrelated authorities in the origin pool and fix EOF liveness.
5. `A3-SEC-04`: total response I/O deadlines.

These changes interact with worker capacity and connection lifecycle. Land them in small independently revertible commits with dedicated fixtures.

## Phase 2 — worker and queue hardening

1. `A3-MEM-01`: length-bounded queue clearing.
2. `A3-SEC-03`: true browser no-network isolation, or disable the feature where unavailable.
3. `A3-SEC-05`: strict incremental worker HTTP framing.

## Phase 3 — deployment-boundary hardening

1. `A3-SEC-06`: SNI/Host binding policy.
2. `A3-SEC-07`: hardened runtime file operations.
3. `A3-DI-01`: hostile-corpus measurement, then decide.

---

# 7. Required validation rails

## 7.1 Existing regression rails

Every accepted patch must continue to run the existing:

* lint and formatting checks;
* C/Rust unit and integration tests;
* Linux x86_64 and arm64 builds;
* macOS Intel and Apple Silicon builds where supported;
* libvips-on and libvips-off builds;
* NGINX module build/smoke matrix;
* Apache module tests;
* sanitizer and fuzz suites;
* release-contract and supply-chain-contract tests;
* Cargo audit;
* CodeQL;
* release image vulnerability scan;
* retained Target-1 memory and static regression rails where relevant.

Do not convert a local fix into a platform-support regression.

## 7.2 New proxy-origin reuse rail

Record for each test cell:

* client RPS;
* p50/p95/p99 latency;
* origin accepts;
* requests per origin connection;
* TLS handshakes;
* origin pool hit/miss/dead/expired/evicted counters;
* CPU;
* RSS;
* `memory.current`;
* lifetime and trial cgroup peaks;
* errors and timeouts.

Cells:

* one HTTP origin;
* one HTTPS origin;
* two alternating origins;
* failover;
* `Content-Length`;
* bodyless;
* chunked;
* origin close;
* downstream early disconnect;
* concurrency from 1 through worker count and above.

The correctness gate is connection reuse without contamination. The performance gate is no regression plus lower connection/handshake churn.

## 7.3 Authentication-abuse rail

Measure valid, known-wrong, and unknown-user attempts under:

* one source;
* many sources;
* no route rate rule;
* a configured route rate rule;
* unrelated public traffic.

Record:

* admitted/rejected pre-auth attempts;
* KDF starts/completions;
* concurrent KDF count;
* CPU/RSS/cgroup;
* public-route RPS and p99;
* auth timing distributions.

## 7.4 Queue rail

For each payload size, record:

* take/publish operations per second;
* lock hold time;
* bytes cleared;
* CPU;
* minor faults;
* RSS and cgroup metrics.

Include a direct stale-data erasure test.

## 7.5 Browser isolation rail

Use canary listeners and network tracing. The acceptance result is binary: captured HTML must produce zero prohibited network connections.

## 7.6 Slow-progress I/O rail

Run more slow origins and slow clients than available workers while continuously probing a normal route. The normal route must remain available within an explicitly documented bound, or admission/backpressure must reject excess work predictably.

## 7.7 Worker HTTP framing rail

A correct framed body must complete without EOF. Every conflicting, truncated, or malformed framing case must fail closed and remain within size/time bounds.

---

# 8. Dependency, CVE, and scanner status

## 8.1 Current conclusion

Audit 3 does **not** issue a fresh “no known vulnerabilities” statement.

The repository commits a Rust lockfile and defines Cargo audit, CodeQL, Dependabot, sanitizer/fuzz, and release-image scanning controls. Audit 2 recorded successful security work and historical scans. Those results are valuable history but are not a current certification for the audited head.

At the audited head:

* Dependabot checks are visible;
* no push-triggered CI/CodeQL run is visible;
* the last observed CodeQL execution failed before analysis;
* this read-only source pass did not execute Cargo audit or an OCI scanner.

Therefore current advisory status is **unverified**, not “clean” and not “known vulnerable.”

## 8.2 Closure evidence required

Before closing audit 3, attach immutable outputs for:

1. `cargo audit --file Cargo.lock` in `workers/laghu-js-optimize`;
2. successful C/C++ CodeQL analysis/upload on the audited remediation commit;
3. sanitizer and fuzz jobs;
4. the release image's fixed-severity and full vulnerability reports;
5. SBOM generation;
6. signature/provenance verification;
7. dependency-review/Dependabot status;
8. any accepted risk, ignore rule, or temporary advisory exception with owner and expiry.

Do not suppress advisories only to obtain green status. Every ignore must record:

* advisory/CVE identifier;
* affected component and reachability;
* compensating control;
* reason upgrade is blocked;
* owner;
* expiry/review date.

## 8.3 Native dependencies

Laghu also relies on system/native components such as OpenSSL, Brotli, zlib, PCRE2, libyaml, libvips, browser packages, NGINX, and Apache according to the selected build/package. Cargo audit alone cannot cover them.

Use the produced SBOM and release image/package scan as the deployable artifact's source of truth. Scanner results from a runner image are not automatically the same as the shipped runtime artifact.

---

# 9. Confirmed strengths and non-findings

The following controls were reviewed and should not be weakened:

* upstream TLS enables peer verification and host/IP identity checks;
* resource-fetch/upload paths use TLS verification and destination restrictions;
* request grammar and framing checks are substantially stronger than in audit 1;
* static-root access uses preopened roots and no-follow/relative-open defenses;
* purge-token reading has explicit metadata/inode checks and secret cleansing;
* configured response-header values reject injection characters during configuration handling;
* browser execution refuses root and retains the Chromium sandbox;
* external process work in the libvips/ffmpeg path is wrapped by an outer process-group timeout/watchdog;
* queue and payload sizes are bounded;
* YAML nesting is bounded;
* actions are pinned by immutable commit SHA;
* release flow includes vulnerability scanning, SBOM, signing, and provenance contracts;
* Cargo.lock is committed;
* sanitizers/fuzzing and multi-architecture test matrices are defined;
* reload generation reclamation and preopened-root lifecycle work from audit 2 remains present.

## Items deliberately not promoted

### ffmpeg/libvips inner timeout

The inner ffmpeg invocation initially appears blocking, but the enclosing isolated-job watchdog kills the process group on timeout. It is not promoted as a new finding.

### Legacy SHA-256 Basic-auth reader

Audit 2 explicitly retained this as a deprecated read-only migration path. Audit 3 does not reopen that decision. New work concerns pre-auth resource bounds and timing equalization.

### Per-request route regex compilation

This remains visible, but the prior precompile experiment regressed the accepted workload and was rejected. It is not repackaged as new work.

### sendfile, accept batching, eager dispatch, allocator, and compiler experiments

All remain closed without new evidence.

### Raw chunked passthrough

The main proxy buffers/dechunks chunked responses. A future validated incremental dechunk-and-stream design might be measurable, but it is too close to previously rejected direct-passthrough work and is not authorized by this audit.

---

# 10. Observability required with accepted fixes

Add bounded counters/histograms rather than verbose per-request logs:

## Origin pool

* acquire hit;
* acquire miss;
* expired;
* dead/EOF;
* unexpected unread data;
* authority-preserved scan;
* release reusable;
* release non-reusable by reason;
* eviction;
* TLS handshake count/duration.

## Authentication

* pre-auth admitted/rejected;
* KDF active/high-water;
* KDF duration;
* unknown/known failure without exposing usernames;
* post-auth rate rejection.

## Queue

* payload length histogram;
* bytes cleared;
* corrupt slot;
* take/publish duration;
* lock hold duration.

## I/O deadlines

* origin-header idle/total timeout;
* origin-body idle/total timeout;
* downstream-write idle/total timeout;
* bytes transferred before timeout.

## Workers

* response framing mode;
* EOF-before-complete;
* framing ambiguity;
* terminal-chunk failure;
* total timeout;
* browser isolation setup failure;
* prohibited browser network attempt if the interception layer can report it safely.

Metrics must remain bounded in cardinality. Do not use raw URL, Host, username, path, IP, or authority values as unrestricted labels.

---

# 11. Security regression principles

Accepted implementations must preserve these invariants:

1. Validation happens before normalization when ambiguity could alter security meaning.
2. HTTP message boundaries are determined by strict framing, not connection folklore.
3. A reusable connection has exactly one fully consumed response and no unread bytes.
4. A blocking operation has an idle bound and a total bound when attacker-controlled progress is possible.
5. Expensive authentication work has cheap admission control and a global resource ceiling.
6. Unknown credentials do not get a materially cheaper failure path.
7. Browser sandboxing and network isolation are separate controls; both are required.
8. Shared runtime paths are secure because code validates trust, not merely because packaging usually creates a restrictive directory.
9. A workflow definition is not an assurance control until it runs and is enforced.
10. Previously rejected performance work is not retried without a new hypothesis and a measurement plan.

---

# 12. Ticket decomposition

Create one work item per primary finding. Do not combine all proxy work into a single unreviewable patch.

Suggested titles:

* `Preserve origin reuse after successful bypass streaming`
* `Stop origin acquisition from draining unrelated authorities`
* `Bound queue payload clearing to consumed bytes`
* `Rate-limit and budget Basic-auth KDF work before scrypt`
* `Equalize Basic-auth unknown-user failure cost`
* `Run Chrome analysis in a no-network isolation boundary`
* `Add total deadlines to origin response and downstream write loops`
* `Use strict incremental HTTP framing in asynchronous fetch workers`
* `Bind downstream SNI and Host in strict multi-site mode`
* `Harden POSIX runtime file and mapping opens`
* `Protect main and enforce CI/security checks`
* `Repair CodeQL native build dependencies`
* `Measure adversarial JavaScript optimizer resource use`

Dependencies:

```text
A3-GOV-02 ──> A3-GOV-01
A3-GOV-01 ──> all merge-gated remediation

A3-SEC-01 ──> A3-SEC-02

A3-RPS-01 ──┐
             ├──> combined origin-pool benchmark
A3-RPS-02 ──┘

A3-SEC-03 has no safe partial deployment:
isolation available ──> enable
isolation unavailable ──> disable/fail closed
```

Each ticket should include its audit ID, evidence paths, acceptance gate, rollback condition, and required metrics.

---

# 13. Disposition ledger

Fill this table as decisions are made.

| ID | Decision | Owner | Target | Evidence / rationale | Closed at commit |
| --- | --- | --- | --- | --- | --- |
| `A3-RPS-01` | Accepted with caveats |  | Upstream origin pool | Fully consumed HTTP/1.1 bypass reuse and non-reuse negatives passed. Local 51/51 CTest; AppleClang ASan/UBSan 2/2 plus smoke; Docker arm64/native AMD64 functional smoke passed with only the reload cgroup numeric assertion disabled. Sequential median 4261.48 to 5490.59 RPS, p95 0.333 to 0.319 ms, p99 0.364 to 0.390 ms; concurrent 64.55 to 66.45 RPS. Limits retained: no TLS numeric benchmark, standard Docker/native cgroup rail stops before RPS, GNU LSan reports 238200 config/core bytes. |  |
| `A3-RPS-02` | Accepted with caveats |  | Shared upstream origin pool | Healthy cross-authority entries survive lookup; explicit EOF/unread/TLS pending handling, bounded shared-pool eviction, A/B HTTP/TLS/failover/concurrency tests, local 51/51, and ASan/UBSan 3/3 passed. Sequential median 4776.47 to 6635.25 RPS; concurrent 6562.40 to 8170.63 RPS; concurrent accepts 718 to 15. Limits: no TLS numeric run; Docker reload cgroup +2.61 MiB; native AMD64 full smoke blocks before fixture on YAML startup; no authority reservation at full shared capacity. |  |
| `A3-MEM-01` | Pending |  |  |  |  |
| `A3-SEC-01` | Accepted |  | Basic-auth admission and global KDF budget | Syntactic Basic-only pre-admission, direct-peer/scope bounded buckets, global KDF rate/burst/concurrency, reload reset, redacted metrics, and post-auth limiter retained. Local `umask 0002` 52/52; Apple ASan/UBSan 51/51; focused Linux AMD64 LSan passed with `detect_leaks=1` and no diagnostic. Docker/native AMD64 SEC smoke reached auth p95 gates, then stopped at the explicitly user-ignored reload cgroup threshold. Full LSan smoke stopped at its existing ASan RSS rail after the auth check; it is not claimed green. |  |
| `A3-SEC-02` | Pending |  |  |  |  |
| `A3-SEC-03` | Pending |  |  |  |  |
| `A3-SEC-04` | Pending |  |  |  |  |
| `A3-SEC-05` | Pending |  |  |  |  |
| `A3-SEC-06` | Pending |  |  |  |  |
| `A3-SEC-07` | Pending |  |  |  |  |
| `A3-GOV-01` | Pending |  |  |  |  |
| `A3-GOV-02` | Accepted implementation; hosted verification pending |  | CodeQL C/C++ | Shared installer + contract; local/Docker/Linux AMD64 validation recorded in §4. Hosted build/analyze/upload, PR/`main`, and publication check pending. |  |
| `A3-DI-01` | Measure first |  |  |  |  |

Allowed terminal decisions:

* **Accepted and verified**
* **Rejected with evidence**
* **Ignored with explicit risk rationale**
* **Deferred with owner and review date**
* **Inapplicable with deployment proof**

“Fixed” without acceptance evidence is not a terminal state.

---

# 14. Audit-3 closure criteria

Audit 3 may be marked **CLOSED** only when:

1. every primary finding has a terminal disposition;
2. every accepted item meets its item-specific acceptance gate;
3. rejected/ignored items include evidence and explicit residual risk;
4. conditional items state whether the relevant deployment feature or trust boundary exists;
5. current CI, CodeQL, Cargo audit, sanitizer/fuzz, and release scanner evidence is attached;
6. `main` protection and required checks are verified, or rejection of that control is explicitly recorded;
7. no accepted audit-1/audit-2 invariant regresses;
8. proxy-origin changes pass connection-isolation and no-cross-response-contamination tests;
9. Chrome analysis is either truly network-isolated or disabled;
10. the final remediation commit SHA and benchmark/scanner bundle are recorded below.

## Final closure record

```text
Audit-3 remediation commit:
Decision date:
Native test bundle:
Proxy-origin benchmark bundle:
Queue benchmark bundle:
Authentication-abuse bundle:
Browser isolation evidence:
Cargo audit evidence:
CodeQL run:
Sanitizer/fuzz runs:
Release image scan:
SBOM/provenance/signature verification:
Main protection/ruleset verification:
Residual accepted risks:
```

---

# 15. Final recommendation

Accept all 12 primary findings, applying the conditional ones only where their documented deployment assumptions hold.

Do not start another broad Target-1 optimization cycle. First restore enforced security gates, correct the existing upstream pool, bound authentication and blocking response work, remove deterministic full-slot queue clearing, and harden the opt-in browser/worker boundaries. These changes have direct correctness, availability, or resource-accounting evidence and are substantially better justified than the experiments already rejected by audits 1 and 2.
