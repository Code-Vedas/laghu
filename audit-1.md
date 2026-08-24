# Laghu performance audit — Pass 1

I audited the current `main` source of `Code-Vedas/laghu`, at commit `4de953b9f3cfce860296310aa87302ee0a71cd48`, following the repository supplied here.  I concentrated on the standalone server, HTTP transaction path, static serving, proxy/origin path, runtime cache/catalog/RUM/queues, HTML/image/JS rewriting, and the performance rail.

The main conclusion is important:

> **The ~50% RPS deficit is not primarily a compiler-optimization problem. Laghu currently has several architectural hot-path costs that are fundamentally different from NGINX. In addition, the existing Target-1 benchmark materially exaggerates the difference because it compares one blocking Laghu worker against an automatically CPU-scaled NGINX.**

There are enough high-impact issues that I would **not** start with LTO, jemalloc, `-march=native`, SIMD, or hand-tuning string functions. Those come later.

## 1. Highest-priority offenders

| Priority | Offender                                                                 |                  RPS impact |        Memory impact | Assessment                   |
| -------- | ------------------------------------------------------------------------ | --------------------------: | -------------------: | ---------------------------- |
| **P0**   | Benchmark: Laghu `workers: 1` vs NGINX `worker_processes auto`           |                     Extreme |                    — | Distorts current comparison  |
| **P0**   | Blocking pthread-per-active-request architecture                         |      Extreme at concurrency |    High scaling cost | Architectural                |
| **P0**   | Standalone sends `Connection: close`                                     | Extreme for small responses |                Minor | Architectural/protocol       |
| **P0**   | Synchronous access logging + `fflush()` per request                      |                        High |                  Low | Immediate fix                |
| **P0**   | Warm responses recompress gzip + Brotli repeatedly                       |  Extreme optimized-mode CPU |       High transient | Clear implementation defect  |
| **P0**   | Chunked responses read as EOF-delimited + 10 MiB dechunk buffer          |    Potentially catastrophic | Up to 10 MiB/request | Correctness + performance    |
| **P0**   | Queue capability snapshot does `flock` + slot scan per optimized request |                        High |                  Low | Hot-path design error        |
| **P1**   | Static/cache hits materialize whole files in heap                        |    High for asset workloads |                 High | Missing zero-copy path       |
| **P1**   | Global proxy mutex shared by unrelated subsystems                        |      High under concurrency |                    — | Contention                   |
| **P1**   | File cache accounting takes cross-process lock on hits                   |                 Medium/high |                    — | Contention                   |
| **P1**   | HTML resource replacement repeatedly rescans/copies whole document       |       High on complex pages |       High transient | Algorithmic                  |
| **P1**   | RUM global mutex + linear slot search                                    |      High on optimized HTML |             Moderate | Algorithmic/contention       |
| **P1**   | Catalog/cache metadata is filesystem-heavy on warm hits                  |                 Medium/high |                  Low | Hot cache isn't actually hot |
| **P1**   | Large configuration structures copied per request                        |                      Medium | Stack/cache pressure | Avoidable                    |
| **P1**   | Route regex compilation/runtime config work on request path              |                      Medium |                  Low | Should be config-time        |
| **P1**   | Rate limiter locks globally and probes up to 256 buckets                 |           High when enabled |                  Low | Configuration-dependent      |
| **P2**   | Per-header `malloc/free`                                                 |     Medium at very high RPS |        Fragmentation | Allocator churn              |
| **P2**   | Multiple repeated linear HTTP-header scans                               |                      Medium |                    — | Parser optimization          |
| **P2**   | No IPO/LTO/PGO/CPU-specific release                                      |                    Moderate |                Small | Do after architecture        |

---

# 2. Fix the benchmark before trusting the 50% number

Your standalone benchmark explicitly generates:

```yaml
workers: 1
connection_queue: 4096
origin_pool_size: 1024
```

The NGINX comparator uses:

```nginx
worker_processes auto;
worker_connections 4096;
access_log off;
```

And Apache is deliberately configured all the way to `MaxRequestWorkers 1024`.

That means on, for example, an 8-core host, the rail can effectively be comparing:

**Laghu: one blocking request-processing thread**

against:

**NGINX: eight event-loop processes**

This alone can explain a substantial portion of an apparent 50%+ deficit.

There is a second asymmetry: Laghu writes an access log record for every request, under its global queue mutex, followed by `fflush(stderr)`. Plain NGINX explicitly disables access logging.

There is a third asymmetry in memory: even Target-1 passthrough initializes Laghu's file cache. The default metadata mmap is 16 MiB.

### I would create two rails

**Normalized single-core rail:** pin every target to one CPU. Laghu 1 worker/reactor, NGINX 1 worker, Apache correspondingly bounded. Same logging policy.

**Production scaling rail:** identical CPU quota for every container. Laghu uses one reactor per assigned CPU, NGINX `auto`, Apache event MPM. Then measure scaling at 1/10/100/1000 concurrent connections.

Also distinguish:

**minimal passthrough:** no transformation queues/cache infrastructure;

from

**production passthrough:** full runtime infrastructure present but transforms bypassed.

Your existing RPS number should not be treated as an architectural truth until this is corrected.

---

# 3. P0: standalone concurrency model

This is the biggest genuine architectural issue.

Laghu currently has a fixed pthread worker pool around a global connection queue. Queue push/pop uses a shared mutex and condition variable, and the worker owns the connection while performing request reading, TLS, upstream work, transformation and response sending.

The normal default is only four workers.

Consequently, with four workers, four slow clients can consume all four execution slots. With the benchmark's one worker, **one** slow client is enough.

NGINX has a fundamentally different model: a worker can simultaneously have thousands of sockets parked in the kernel readiness mechanism.

### Target architecture

For standalone Laghu I would move toward:

```text
                  ┌──────────── reactor 0 ────────────┐
accept / reuseport ──────────── reactor 1 ────────────┤
                  ├──────────── reactor 2 ────────────┤
                  └──────────── reactor N ────────────┘
                               │
                      asynchronous sockets
                               │
                CPU-heavy optimization jobs
                               │
                     dedicated worker queues
```

On Linux, `epoll`; on BSD/macOS, `kqueue`, hidden behind a small readiness abstraction.

Use approximately **one reactor per CPU**. CPU-heavy transforms remain separate jobs.

A transitional change of merely making `workers = online_cpu_count()` will improve numbers, but it is not the final fix. A 32-thread blocking server still handles idle keep-alive connections fundamentally worse than a four-reactor event server.

---

# 4. P0: Laghu defeats HTTP/1.1 connection reuse

The standalone response path unconditionally emits:

```http
Connection: close
```

k6 normally reuses HTTP connections. Your NGINX peer therefore gets to amortize:

TCP setup → many requests

while Laghu repeatedly performs:

TCP setup → one request → close
TCP setup → one request → close
TCP setup → one request → close

This hurts tiny static files and `/redirect` disproportionately.

It also makes the benchmark less about application RPS and more about connection establishment.

### Required change

Support HTTP/1.1 persistent connections, with bounded:

`keepalive_idle_timeout`, `keepalive_requests`, parser/request-buffer reuse and explicit handling of client `Connection: close`.

But this should be implemented together with readiness-based I/O. Adding keep-alive to the existing blocking pool would let idle clients monopolize worker threads.

This is one of the few changes I would expect to make an immediately visible difference in Target-1.

---

# 5. P0: synchronous logging is sitting directly in the hot path

The standalone logger currently:

takes the global queue mutex → formats the event → `fwrite()`/`fputc()` → `fflush(stderr)` → releases lock.

That creates three different costs:

**Serialization.** Concurrent requests eventually converge on one mutex.

**Formatting CPU.** Structured text formatting is done before every completion.

**Kernel/container I/O.** `fflush()` deliberately defeats stdio batching.

For a benchmark trying to measure tens/hundreds of thousands of simple operations, this is expensive.

### Replace it with

```text
request reactor
    │
    ├── write small record to bounded per-worker ring
    │
    └── continue immediately

                     log consumer
                          │
                    batch formatting
                          │
                     batched write
```

At minimum, add `access_log: off` and turn it off for the equivalent NGINX comparison.

For production, use bounded asynchronous logging with a defined overflow policy. Do not let the logging consumer create unbounded memory growth.

---

# 6. P0: warm responses are recompressed

This was one of the strongest findings in the optimized path.

HTTP finalization runs the precompression path.

`laghu_precompressed_publish()` then:

1. SHA-256 hashes the representation.
2. gzip-compresses it.
3. Brotli-compresses it.
4. publishes the artifacts.

The gzip implementation uses **`Z_BEST_COMPRESSION`** and Brotli is quality **8**.

Critically, there is no inexpensive variant-exists check **before** performing both compressions.

So a warm request can still pay expensive gzip+Brotli computation merely to rediscover/publish an artifact already available.

That can crush optimized-mode RPS.

### Correct model

```text
body identity/hash
       │
       ├── compressed variant exists ──► serve directly
       │
       └── missing
              │
        single-flight enqueue
              │
        serve identity now
              │
       background compression
              │
        publish immutable result
```

Compression belongs on the asynchronous derivation side, especially Brotli quality 8.

There should also be a **single-flight** mechanism so 100 simultaneous first-hit requests do not launch 200 encodes.

---

# 7. P0: chunked upstream response handling

The proxy body path treats responses without `Content-Length` as read-until-close, then chunked bodies are decoded afterward. The decode allocation is based on `LAGHU_PROXY_MAX_BODY`, which resolves to the image maximum of **10 MiB**.

This causes two problems.

For an HTTP/1.1 chunked response, the message finishes at the `0\r\n...\r\n` terminating chunk. It does **not** require the origin to close the connection. Reading until EOF can therefore pin a blocking worker waiting for a connection that the origin is correctly keeping alive.

Then the entire encoded body is followed by another allocation potentially as large as 10 MiB for decoding.

### Rewrite this as a streaming state machine

```text
chunk-size
   ↓
chunk-data
   ↓
CRLF
   ↓
next size
   ↓
...
   ↓
zero chunk
   ↓
trailers
   ↓
message complete
```

For bypass responses, decoded chunks can flow downstream immediately.

For responses needing capture, append to a bounded actual-size buffer.

Do **not** allocate 10 MiB merely because that is the maximum permitted representation.

This one is both a performance defect and a correctness issue that I would repair before scaling the proxy.

---

# 8. P1: static serving copies everything through userspace heap

The standalone static path opens/walks the requested file and reads the complete representation into allocated memory before serving it.

So a 100 KiB asset can look approximately like:

```text
open/openat
     ↓
malloc(100 KiB)
     ↓
read file → heap
     ↓
possibly allocate transformed/compressed buffer
     ↓
send heap → socket
     ↓
free
```

The ideal no-transform path is closer to:

```text
validated fd + metadata
          ↓
       headers
          ↓
sendfile(fd → socket)
```

For TLS, true `sendfile()` generally cannot bypass userspace encryption, but Laghu can still use reusable per-reactor buffers rather than one malloc per file.

An important nuance: your plain NGINX benchmark does not explicitly enable `sendfile on`, so I would consider zero-copy serving an avenue for improvement rather than claim it currently explains the benchmark gap.

Also cache site document-root descriptors and avoid repeatedly reopening the same root path.

On Linux, `openat2()` with appropriate `RESOLVE_*` restrictions would give both speed and a cleaner secure path-resolution primitive; retain a portable safe fallback.

---

# 9. P1: your cache hit path is still expensive

The cache is durable, but it is not yet functioning like a high-speed serving cache.

`laghu_runtime_cache_entry` ultimately references an artifact file.

Cache metadata lookup can involve filesystem metadata and file reads; the artifact lookup performs metadata-file size/read and payload-file size validation.

JavaScript cache hits then do:

```c
malloc(entry.length)
laghu_runtime_cache_read(...)
```

So a warm cache hit can still mean:

```text
hashing
metadata pathname construction
stat/open/read/close metadata
stat artifact
malloc(payload)
open/read/close payload
send
free
```

That is nowhere close to an NGINX-style hot static response.

Interestingly, your API already anticipates the right design: `laghu_http_transaction_result` has `cached_entry` and explicitly documents that immutable cache artifacts may be sent without copying payload bytes.

The standalone adapter should take advantage of that.

### Better cache tiers

```text
L1: per-reactor hot metadata/fd cache
          ↓ miss
L2: mmap/shared metadata index
          ↓ miss
L3: immutable artifact filesystem
```

Serve cache artifacts by fd rather than converting them into request-owned heap buffers.

Also, hit/miss accounting should not require synchronizing the underlying shared cache on every lookup.

---

# 10. P1: file-cache accounting causes cross-process synchronization

The file cache's hit/miss path updates shared metadata using `laghu_runtime_shared_mapping_try_lock()`. On POSIX the shared-mapping lock is implemented using `flock`.

A cache hit should not require a cross-process file lock just to increment statistics.

Instead:

```text
reactor-local counters
        │
        │ no lock
        ▼
periodic aggregation
        │
        ▼
shared operational state
```

Access-time/touch information can likewise be quantized. Updating “last used” once every 30–60 seconds for a hot object is sufficient for eviction logic; it does not need to become request synchronization.

---

# 11. P1: optimized requests scan the complete worker queue

This one is particularly avoidable.

`proxy_runtime_queue_capabilities()` obtains a runtime queue snapshot on the request path.

The snapshot implementation takes the shared-mapping lock and iterates over all slots to calculate occupancy.

So optimized request preparation can effectively do:

```text
request
  ↓
flock(queue)
  ↓
scan every queue slot
  ↓
read capabilities / heartbeat
  ↓
unlock
```

Capabilities and worker heartbeat are not request-scoped information.

The header itself says:

> “Worker capability state refreshed by the adapter lifecycle, never by a request.”

The standalone implementation is violating the intended architecture.

Refresh the snapshot periodically—100 ms, 500 ms or one second depending on desired responsiveness—and expose a cached atomic capability mask/health state to request handling.

---

# 12. P1: HTML rewrite complexity becomes multiplicative

`laghu_runtime_replace_cached_urls()` is especially expensive.

It starts by copying the complete document. Then, for **each resource**, it scans the complete current HTML to count occurrences, allocates another complete output buffer, scans the HTML again to perform replacement, frees the previous buffer, and moves to the next resource.

For `R` resources and `H` bytes of HTML, that behaves approximately as:

**O(R × H)** scans, plus repeated full-document copying.

And that's after other HTML/resource discovery passes have already occurred.

### Replace with one parser/token stream

Build:

```text
URL → replacement URL
```

in a hash table.

Then tokenize HTML once and rewrite relevant attribute values as they are encountered.

The same parsing pass could eventually feed:

image discovery, stylesheet discovery, JavaScript discovery, resource hints, image rewriting, CSP-related observations.

Right now several subsystems independently walk much of the same byte stream.

The markup builder also begins at only 256 bytes and grows via reallocating powers of two.

For HTML where the input length is already known, reserve roughly:

`input_length + estimated_expansion`

upfront.

---

# 13. P1: RUM lookup is global-lock + linear-search

Every RUM read takes one engine-wide pthread mutex.

Then `laghu_rum_find()` linearly scans all slots:

```c
for (index = 0; index < engine->slot_count; ++index)
    ...
```

HTML rewriting can perform RUM lookups for discovered image resources, turning a complex page into repeated:

```text
mutex
linear scan
mutex
linear scan
...
```

under concurrency.

Replace this with a keyed structure.

A good implementation would be a sharded hash table or read-mostly immutable snapshot/RCU arrangement. Access timestamps should not force the read operation onto an exclusive lock.

---

# 14. P1: catalog reads are not warm-memory operations

Catalog lookup builds a key, opens a metadata file, reads the record, closes it, calculates SHA-256 across the stored record, validates it and then returns it.

HTML can perform this per discovered resource.

The checksum is useful for persistence/recovery, but recomputing it after every successful hot lookup is expensive insurance.

I would maintain catalog metadata in a bounded mmap/in-memory hash structure and validate durable records as they are loaded/published/recovered.

---

# 15. P1: route configuration is runtime data instead of compiled runtime state

Route handling currently has request-time regex compilation/execution/freeing and repeated traversal of site/routes for different decisions.

Configuration doesn't change for every request.

Turn this:

```text
YAML/config structs
       ↓
request repeatedly interprets them
```

into:

```text
YAML/config structs
       ↓
config validation
       ↓
compile
       ↓
immutable runtime configuration
       ↓
very cheap request lookup
```

That compiled object should contain host lookup table, exact-path hash, prefix/radix structure, precompiled ordered regexes, resolved policy, pattern lengths, MIME representation, feature masks and upstream keys.

Query overrides can remain a deliberately slower exceptional path.

---

# 16. P1: `laghu_config` is copied unnecessarily

`laghu_config` is not small. It contains, among other things, a 2 KiB MIME allowlist, resource rules and domain policy data.

`laghu_http_environment` embeds `laghu_config` **by value**, and a transaction then embeds/copies the environment.

The standalone proxy explicitly constructs:

```c
.config = *config
```

and HTTP prepare subsequently copies the environment into the transaction.

These are static data.

Use something closer to:

```c
const laghu_compiled_config *config;
const laghu_compiled_policy *policy;
```

in request state.

This reduces memory traffic, stack/request-context size and CPU cache pollution.

---

# 17. P1: origin pooling is globally coupled

Origin-pool acquisition runs under the shared proxy mutex and iterates a global origin array. Each examined candidate is removed from the array before matching/reuse succeeds.

Thus pools for unrelated authorities contend with each other, and examining one authority can churn connections belonging to others.

Additionally, upstream health lookup/update also reaches the global proxy queue lock.

Partition origin pools by upstream identity:

```text
scheme + host + port + TLS configuration
```

and preferably make them reactor-local.

That simultaneously removes synchronization and improves connection locality.

DNS should also be resolved/refreshed outside individual request execution rather than relying on fresh blocking resolution when a new connection is necessary.

---

# 18. P1: rate limiting becomes global serialization

On a rate-limited route, `proxy_rate_allowed()` takes the global proxy queue lock and can probe the entire 256-entry rate table.

The configured table is only 256 buckets.

This is fine at low traffic but becomes a contention point exactly when rate limiting is most useful.

Use sharded token buckets/hash partitions, ideally local to a reactor with carefully defined global approximation semantics if exact global rates are unnecessary.

---

# 19. P2: allocator churn throughout the HTTP result path

Generated response headers each own separately allocated values. `laghu_http_add_header_operation()` allocates one buffer per value, and result teardown loops over them and frees each one.

Even ordinary Laghu status decoration creates multiple `X-Laghu*` headers.

At 100k RPS, “three tiny mallocs and three frees” stops being tiny.

Use a request arena:

```text
request_scratch {
    headers
    formatted strings
    transform scratch
    parser scratch
}
```

and reset it in O(1) when the request completes.

For short header values, inline fixed/small storage may be even simpler.

Do this **after** eliminating the much larger body allocations.

---

# 20. Memory-specific findings

The biggest memory improvements are not about squeezing structs by 8 bytes.

Current transient allocations can include source response body, transformed body, compressed output, cached representation and—in the chunked case—a potential 10 MiB decode area.

A single optimized response can therefore momentarily own several copies of the same logical payload.

The desired ownership model is:

```text
network/file input
       │
       ├──── borrowed / streamed
       │
 transformation only if needed
       │
       ├──── one bounded scratch arena
       │
 immutable published artifact
       │
       └──── referenced, not copied
```

The 16 MiB default metadata mapping is another fixed RSS/cgroup cost.  It should be sized according to expected metadata/inode count instead of being an unavoidable baseline for a minimal passthrough process.

One benchmark detail matters here: your rail records both cgroup peak and summed per-process `VmRSS`.  For multi-process targets, summed RSS can double-count shared mappings/pages. **Cgroup memory should be the stronger “how much memory did this target actually consume?” metric.**

---

# 21. What I would change, in exact order

|  Order | Change                                                      | Why first                                       | Status (2026-08-24) |
| -----: | ----------------------------------------------------------- | ----------------------------------------------- | ------------------- |
|  **1** | Repair benchmark CPU/log/cache equivalence                  | Establish trustworthy baseline                  | **Applied.** Matched rail controls and diagnostics are in place; ARM results remain diagnostic until native AMD64. |
|  **2** | Make access logging off/async; remove `fflush` request path | Very low-risk high reward                       | **Applied for Target 1.** `runtime.access_log: off` removes record/flush work; asynchronous production logging is not implemented. |
|  **3** | Stop recompressing existing gzip/Brotli artifacts           | Huge optimized-mode CPU waste                   | **Not applied.** Deferred to Target 4 because it is optimization-only work. |
|  **4** | Cache queue capability/heartbeat outside request path       | Removes `flock` + queue scan                    | **Not applied.** Deferred to Target 4 because it is optimization-only work. |
|  **5** | Implement correct incremental chunk parser                  | Fixes worker stalls + 10 MiB allocation         | **Applied.** Bounded incremental parser stops at terminal chunk/trailers; removes EOF wait and fixed 10 MiB decode allocation. |
|  **6** | Build event-driven downstream connection layer + keep-alive | Fundamental RPS scalability                     | **Not applied.** Probe/candidate regressed static RPS, so it was discarded. |
|  **7** | Add direct static/cache artifact serving                    | RPS + allocation/RSS gain                       | **Partial.** Direct fd/sendfile candidate failed cgroup gate; retained mode-off bypass removes per-request transform-context allocation. |
|  **8** | Split global proxy mutex / reactor-local origin pools       | Scaling after multiple reactors                 | **Not applied.** Deferred to Target 4: Target-1 performance cells do not exercise upstream proxy traffic. |
|  **9** | Compile routing/policies/config once                        | Strip request setup cost                        | **Not applied.** Regex precompile improved regex-only work but regressed baseline/prefix cells; discarded. |
| **10** | Make cache/catalog metadata truly hot                       | Remove stat/open/read/checksum pressure         | **Not applied.** Deferred to Target 4 because it is optimization-only work. |
| **11** | Replace RUM mutex+linear scan                               | Optimized HTML concurrency                      | **Not applied.** Deferred to Target 4 because it is optimization-only work. |
| **12** | Merge HTML discovery/rewrite into one pass                  | Large complex-page improvement                  | **Not applied.** Deferred to Target 4 because it is optimization-only work. |
| **13** | Introduce request arenas/reusable buffers                   | Remove allocator churn                          | **Partial.** Mode-off static path now bypasses transform-context allocation; broader request arenas/reusable buffers are not implemented. |
| **14** | Add ThinLTO/LTO + PGO                                       | Exploit cleaner architecture                    | **Not applied.** Both LTO and PGO regressed focused RPS measurements, so they were discarded. |
| **15** | Evaluate jemalloc/mimalloc, CPU tuning                      | Only after allocation architecture is corrected | **Not applied.** jemalloc raised memory about 22–30%, failing the memory gate; discarded. |

---

# 22. Compiler/build tuning comes last, but should still be done

The standalone benchmark does correctly build CMake `Release`; this is not a debug-build problem.

But the top-level build currently has no project-level IPO/LTO, PGO or CPU tuning.

After the P0/P1 work, I would benchmark:

**ThinLTO/LTO** for production release builds.

**PGO** trained on your actual rail mixture: static 1K/100K, redirect, proxy, cache hit, HTML optimized hit, image artifact hit.

**CPU target builds** such as x86-64-v3 where your distribution contract allows it. Keep portable binaries separately.

**Allocator experiments** only after removing the pathological allocation volume. An allocator can make `malloc` faster; it cannot make copying 10 MiB unnecessary.

---

# Expected diagnosis of the current 50% gap

I would currently divide your observed problem into three categories.

**Benchmark-induced gap:** potentially very large. `workers: 1` versus NGINX `worker_processes auto`, plus Laghu synchronous access logs versus NGINX logging disabled, is not a fair server-efficiency comparison.

**Real standalone-server gap:** also large. Blocking connection ownership, forced connection close, global coordination and full-body materialization prevent Laghu from having NGINX-like concurrency economics.

**Optimization-engine gap:** substantial in optimized workloads. Repeated precompression, filesystem-heavy “cache hits”, queue snapshot locking, RUM lookup, catalog reads and repeated HTML scans make the warm path much more expensive than it should be.

So I would **not** conclude that Laghu's underlying transformation algorithms are simply “50% slower.” The server is currently paying a considerable amount of orchestration work that a mature high-performance HTTP server avoids altogether.

## The performance end-state

The architecture I would aim for is:

```text
                  ┌───────────────────────────────┐
                  │  N event-loop/reactor workers │
                  └──────────────┬────────────────┘
                                 │
             ┌───────────────────┼────────────────────┐
             │                   │                    │
        static/cache         proxy I/O          admin/beacon
             │                   │
       zero/one-copy       async origins
             │                   │
             └──────────────┬────┘
                            │
                immutable compiled policy
                            │
               cheap cache/catalog/RUM reads
                            │
                 ┌──────────┴───────────┐
                 │ asynchronous derivation
                 │ image / JS / Brotli
                 └──────────────────────┘
```

The crucial rule is:

> **A warm ordinary request should execute almost no allocation, no file lock, no configuration interpretation, no compression, and no full-document transformation.**

That is the standard I would use for every subsequent optimization decision.

This is a **source audit**, not a `perf`/flamegraph capture, so I am deliberately not assigning invented percentage improvements to individual fixes. After correcting the rail, the next performance iteration should use `perf`, syscall counts, allocation profiling and mutex-contention measurements to prove which of these dominates on the actual machine.

I have intentionally kept the security audit out of this report so performance findings do not get diluted. The next pass can audit memory safety, HTTP parsing/framing, path traversal/symlinks, proxy/request smuggling, TLS, authentication/token handling, SSRF/upstreams, cache poisoning, command/file handling, privilege boundaries, dependencies and compiler hardening as a separate offender report.

---

# 23. Target 1 native AMD64 result (2026-08-24)

Native acceptance run: 99/99 clean trials on `linux-fast-build` (Linux AMD64), three medians per cell, matched four-CPU production-scaling rail and access logging off. Raw after bundle: `tmp/benchmarks/native-amd64-target1-after-20260824/results.json`; it records image/config digests and zero HTTP/check errors.

The retained before bundle is `tmp/benchmarks/native-amd64-target1-before-retained-20260823/results.json` (99/99 clean, same host and corpus). It predates the explicit four-CPU/logging-equivalence metadata, so before/after deltas below show measured directional gain, not a replacement acceptance verdict.

| Standalone cell | RPS change | cgroup change | RSS change |
| --- | ---: | ---: | ---: |
| warm 1 | +33.2% | +2.7% | +3.2% |
| warm 10 | +90.2% | +1.7% | +3.6% |
| warm 50 | +61.4% | +2.1% | +3.6% |
| warm 100 | +17.8% | +2.1% | +3.3% |
| warm 500 | +54.6% | +0.7% | +3.4% |
| warm 1000 | +74.9% | +1.8% | +3.5% |
| JavaScript execution 10 | +232.3% | +1.8% | +3.5% |
| mixed assets 1000 | +42.1% | +0.8% | +3.8% |
| cache storm 1000 | +48.8% | +3.2% | +3.9% |
| cache thrash 1000 | +69.7% | +3.2% | +4.0% |
| soak 1000 | +27.4% | +9.4% | +3.9% |

Current native Target-1 acceptance is **fail**: versus NGINX, 0/11 cells pass (RPS 33.4–132.5%, cgroup 236.4–345.3%, RSS 113.8–114.9% of baseline); versus Apache, 4/11 cells pass (RPS 51.7–112.3%, cgroup 55.0–138.7%, RSS 45.3–105.9%). Target 1 therefore remains unaccepted; the persisted raw after bundle is the new native baseline.

ARM was deliberately not run for this remeasurement. Target 2 rail/configuration is ready but has not been run or changed.

---

# 24. Target 1 detailed handoff for the next audit (2026-08-24)

This section is an evidence handoff, not a new claim of parity.  It distinguishes observed data from interpretation and recommended next work.  Section 21 remains the concise priority-1-to-15 disposition; this section adds the details needed to audit those decisions without changing that table.

## Observed facts: locked Target 1 contract

Target 1 compares standalone Laghu with no optimizations against plain NGINX and plain Apache.  It uses matched static delivery, virtual-host selection, proxy and redirect correctness routes, corpus, headers, container limits, warm-up policy, and activation rules.  Standalone memory means only the standalone Laghu container; it excludes the NGINX origin container.

The acceptance unit is one equivalent load cell, never an aggregate.  Each cell is three clean independent trials; its value is the median.  Every trial must have zero HTTP/check errors.  A standalone cell passes a reference only when RPS is at least 98% of that reference and both peak cgroup memory and peak process RSS are at most 102% of it.  Faster RPS or lower memory is always favorable, but does not excuse failure of another gate.

| Cell | Request path(s) | VUs | Work shape |
| --- | --- | ---: | --- |
| warm 1 | `/index.html` | 1 | fixed 1,000 iterations |
| warm 10 | `/index.html` | 10 | fixed 1,000 iterations |
| warm 50 | `/index.html` | 50 | fixed 1,000 iterations |
| warm 100 | `/index.html` | 100 | fixed 1,000 iterations |
| warm 500 | `/index.html` | 500 | fixed 1,000 iterations |
| warm 1000 | `/index.html` | 1,000 | fixed 1,000 iterations |
| JavaScript execution 10 | `/js-10k.js` | 10 | fetch then execute JavaScript |
| mixed assets 1000 | `/index.html`, `/css-100k.css`, `/js-100k.js`, `/image-480.jpg` | 1,000 | round-robin assets |
| cache storm 1000 | `/image-480.jpg` | 1,000 | repeated same image |
| cache thrash 1000 | `/image-100.jpg`, `/image-480.jpg`, `/image-768.jpg`, `/image-1440.jpg`, `/image-3840.jpg` | 1,000 | round-robin image set |
| soak 1000 | same four mixed-asset paths | 1,000 | 30-second sustained load |

The accepted native execution identity is `tmp/benchmarks/native-amd64-target1-after-20260824/results.json`.  It contains 99 raw trials: three targets × eleven cells × three repetitions.  Its execution record says `native-linux-amd64`, `linux/amd64`, host `Linux-6.12.101+deb13-amd64-x86_64-with-glibc2.41`, machine `x86_64`, and `linux-fast-build`.  All three measured target containers report `linux/amd64`; `errors` is an empty array.

The rail is `production-scaling`: four CPU quota units, four standalone workers, NGINX `worker_processes auto`, and Apache `benchmark-mpm-production-scaling.conf`.  Request headers are `Host: alpha.bench.test`, `Accept: */*`, and `Accept-Encoding: identity`.  Request-access logging is disabled for all targets; standalone retains lifecycle and error diagnostics on stderr.  The result also records corpus SHA-256, config SHA-256 values, and image digests, so an auditor should read that JSON rather than reproduce digests in prose.

The only retained "before" comparison is `tmp/benchmarks/native-amd64-target1-before-retained-20260823/results.json`.  It is a clean 99-trial same-host/corpus bundle, but it predates explicit four-CPU and logging-equivalence metadata.  Its directional deltas in section 23 are useful evidence of improvement; they are not a replacement Target-1 acceptance baseline and must not be used to relax the current gate.

## Observed facts: current per-cell parity

Numbers below are standalone divided by the named plain-server median.  RPS must be at least 98.0%; cgroup and RSS must each be at most 102.0%.  “RPS”, “CG”, and “RSS” in the failure column name only the threshold(s) exceeded; they do not assert a source-level cause.

| Cell | vs NGINX: RPS / CG / RSS | NGINX gate result | vs Apache: RPS / CG / RSS | Apache gate result |
| --- | --- | --- | --- | --- |
| cache storm 1000 | 101.9% / 336.5% / 114.3% | fail: CG, RSS | 112.3% / 64.5% / 45.8% | pass |
| cache thrash 1000 | 132.5% / 312.7% / 114.0% | fail: CG, RSS | 92.2% / 63.3% / 45.7% | fail: RPS |
| JavaScript execution 10 | 124.0% / 345.3% / 114.3% | fail: CG, RSS | 106.8% / 64.4% / 46.3% | pass |
| mixed assets 1000 | 88.6% / 333.8% / 114.3% | fail: RPS, CG, RSS | 111.4% / 64.3% / 45.9% | pass |
| soak 1000 | 49.0% / 236.4% / 113.8% | fail: RPS, CG, RSS | 63.4% / 55.0% / 45.3% | fail: RPS |
| warm 1 | 85.3% / 326.0% / 114.0% | fail: RPS, CG, RSS | 103.4% / 138.7% / 105.9% | fail: CG, RSS |
| warm 10 | 42.3% / 327.2% / 114.9% | fail: RPS, CG, RSS | 64.9% / 127.9% / 100.6% | fail: RPS, CG |
| warm 50 | 80.3% / 327.9% / 114.8% | fail: RPS, CG, RSS | 83.1% / 121.8% / 92.5% | fail: RPS, CG |
| warm 100 | 33.4% / 327.9% / 114.4% | fail: RPS, CG, RSS | 51.7% / 110.1% / 82.1% | fail: RPS, CG |
| warm 500 | 63.7% / 328.7% / 114.5% | fail: RPS, CG, RSS | 73.8% / 79.0% / 57.6% | fail: RPS |
| warm 1000 | 80.6% / 345.3% / 114.6% | fail: RPS, CG, RSS | 106.9% / 64.4% / 47.8% | pass |

“0/11 against NGINX” means zero of the eleven independent cells meets all three gates against NGINX.  NGINX cgroup memory fails in every cell (236.4–345.3% of NGINX), and standalone RSS also fails in every cell (113.8–114.9%); eight cells additionally fail RPS.  It does not mean zero requests succeeded: all 33 NGINX-comparison trials and their standalone counterparts completed with zero reported errors.

“4/11 against Apache” means four cells meet all three gates: cache storm, JavaScript execution, mixed assets, and warm 1000.  The remaining seven miss RPS, cgroup memory, RSS, or a combination as shown above.  Thus standalone is not yet accepted even though it is already better than Apache in some cells and dramatically smaller than Apache in several high-concurrency cells.

## Observed facts: retained work and measured focused evidence

* **Benchmark controls and logging equivalence.** The focused rail now emits the one locked target inventory, raw trials, medians, per-cell ratios, thresholds, errors, host/config/image provenance, and no overall-winner field.  The native result above is the first retained Target-1 result with those explicit production-scaling and logging-equivalence fields.
* **Access logging off for Target 1.** `runtime.access_log: off` skips request transaction rendering, mutex/write/flush work while retaining lifecycle and error diagnostics.  Local ARM focused A/B evidence in `tmp/benchmarks/local-arm64-access-log-ab-20260823/` measured cache-storm RPS 2,921→3,270 (+11.9%) and soak 5,370→5,938 (+10.6%); cgroup fell about 31–33%, while RSS was effectively unchanged.  These focused ARM results explain retention but are not native AMD64 acceptance evidence.
* **Incremental bounded chunked-upstream parser.** The native transport now stops at the terminal chunk/trailers and grows decode storage within the body limit rather than allocating a fixed 10 MiB buffer and waiting for origin EOF.  Focused local evidence in `tmp/benchmarks/local-arm64-target1-chunked-probe-20260823/` and `tmp/benchmarks/local-arm64-target1-chunked-fix-20260823/` reduced its test fixture from 258.9 ms to 3.925 ms and raised it from 3.863 to 254.799 RPS.  This is retained for correctness and proxy performance, but Target-1 performance cells are static/plain and do not use it.
* **Mode-off static bypass.** The static path bypasses the shared transform finalizer when the resolved mode is off, preserving static request semantics and headers.  Focused three-pass local A/B evidence in `tmp/p13-mode-off-ab/raw/` measured tiny HTML 5,669.83→7,086.65 RPS (+25.0%) and 100 KiB CSS 4,659.12→6,982.27 (+49.9%), with slightly lower cgroup/RSS.  This is the relevant retained speed change for Target 1; its full native effect is represented only by the final 99-trial bundle.
* **Static-path truncation fix.** Oversized constructed paths now return 414 instead of serving a truncated prefix; regression coverage proves trailing-slash and SPA-fallback cases never serve the truncated-prefix resource.  This is a correctness retention, not a parity claim.
* **Validation status.** The retained tree previously completed default Release build, 48/48 CTests, NGINX/Apache smoke lanes, docs, formatting, and `scripts/run-all`; the native performance bundle itself contains zero HTTP/check errors.  No permanent probe output/code is retained in product source.

## Observed facts: rejected or deferred work

* **Priority 6, downstream keep-alive/repark session candidate: rejected.** Its static focused RPS regressed about 61%; raw connection-lifecycle probe evidence remains at `tmp/connection-lifecycle-probe/20260824T130500Z/`.  That probe found low queue mutex wait in its small fixture, so it did not justify retaining a regressing architecture candidate.
* **Priority 7, fd/sendfile static candidate: rejected.** It gained plaintext static RPS by about 25–36%, but the 100 KiB plaintext cgroup median rose 14.9%, far beyond the 2% gate; RSS was essentially unchanged.  Raw A/B evidence is `tmp/p7-static-cache-ab/20260824T155200Z/`; later cgroup probe data is `tmp/static-cgroup-probe/20260824T165000Z/`.  The later probe attributed only 64 KiB to the worker buffer and did not prove the earlier approximately 5 MiB peak difference as a durable component allocation.
* **Priority 9, route/regex precompile candidate: rejected.** It improved regex CSS from 105.12 to 167.50 RPS (+59.3%), but tiny static fell from 698.33 to 464.17 RPS (-33.5%) and prefix CSS from 151.39 to 133.04 RPS (-12.1%).  Raw evidence is `tmp/route-policy-precompile/20260824T172922Z/results.json`; the narrow repeat is `.../20260824T173707Z/results.json`.  It did not provide complete retained-memory evidence for every cell, so it must not be treated as a memory conclusion.
* **Priority 14, GCC IPO/LTO candidate: rejected.** On its ARM focused A/B, tiny HTML, 100 KiB CSS, and mixed assets regressed 7.0%, 15.1%, and 10.3% RPS respectively; p95 worsened in every cell while cgroup/RSS stayed effectively flat.  Raw data is under `tmp/p14-thinlto-ab/raw/`; this was GCC IPO/LTO, not LLVM ThinLTO.
* **Priority 14, PGO candidate: rejected.** ARM focused data in `tmp/p14-pgo-ab/summary.json` shows CSS 4,868.57→3,240.32 RPS (-33.4%), mixed assets 5,901.75→5,505.24 (-6.7%), and tiny HTML essentially flat/slightly lower.  It has zero reported HTTP/check errors, but no full rail and no retention case.
* **Priority 15, jemalloc candidate: rejected.** Its three focused cells raised cgroup memory 24.7–29.7% and RSS 21.6–28.3%, violating the 2% memory gate in every cell.  `tmp/p15-allocator-ab/summary.json` is the raw summary.  Its apparent CSS RPS gain is not an acceptance result because the system-control CSS run was order-biased/anomalously slow; the memory failure is still sufficient to reject the candidate.
* **Optimization-only priorities 3, 4, 8, 10, 11, and 12: deferred.** Recompression, queue capability snapshots, origin-pool/mutex work, cache/catalog work, RUM, and HTML rewrite work belong to Target 4 or proxy/optimization paths.  They are not a credible explanation for static Target-1 parity and must not be pulled forward merely because they are visible in a broad source audit.

## Interpretation: what the native result supports, and what it does not

1. **Fixed/runtime memory is the first remaining Target-1 problem.** Standalone is approximately 78.6–96.3 MiB cgroup and 54.8–55.5 MiB summed process RSS over the eleven medians, even before higher-concurrency differences are considered.  NGINX is about 24.1–40.7 MiB cgroup and 47.9–48.8 MiB RSS.  The nearly flat standalone RSS and cgroup across most warm VU levels, plus failure of NGINX memory gates in every cell, point first to initialized runtime/worker/cache infrastructure rather than a request-sized leak.  This is an inference; the final rail does not yet decompose its memory by mapping, queue, worker stack, cache, or socket state.
2. **The retained mode-off bypass removed one real request allocation but cannot by itself close the fixed gap.** Temporary request-allocation evidence at `tmp/request-allocation-probe/20260824T180243Z/results.json` recorded one 117,936-byte transform-context allocation per static request before that bypass.  The native result still has the fixed memory pattern, so an auditor should look for process-start and worker-start allocation rather than re-open that already-bypassed finalizer path first.
3. **RPS is the second Target-1 problem after memory decomposition.** Standalone already exceeds NGINX RPS in cache storm, cache thrash, and JavaScript execution, yet is only 33.4–88.6% in mixed/most warm cells and 49.0% in soak.  The shape rules out a single universal "Laghu is slow" explanation.  It is consistent with connection lifecycle, worker scheduling, static read/send behavior, and workload-specific contention, but no native flamegraph, syscall profile, or production-load contention probe has proven one of those causes.
4. **The static send candidate is not evidence that sendfile is the next safe fix.** Its RPS gain was real in its focused setup, but its cgroup failure makes it ineligible.  The later probe weakened the first explanation for that increase; it did not establish a replacement cause.  Reintroducing sendfile or a large buffer without a fresh probe would repeat an already rejected path.
5. **Chunked upstream, catalog, RUM, HTML, and origin-pool work must stay separate.** Target 1's benchmark cells test no-optimization standalone static delivery.  The proxy and transformation improvements are useful product work but do not explain the present static/Nginx memory gate unless a probe proves shared initialization is responsible.

## Priority disposition cross-check

For audit traceability, section 21's concise rows have this current interpretation: 1 is complete benchmark-equivalence work; 2 and 5 are retained; 3, 4, 8, 10, 11, and 12 are deferred to Target 4; 6 and 9 are rejected after focused regression; 7 and 13 are partial through the retained mode-off bypass while their broader candidates remain rejected/not implemented; 14 and 15 are rejected.  No priority is silently considered accepted without the evidence described above.

## Recommendation: next Target-1 cycle and approval boundary

Remain on Target 1.  Do not repair or run Target 2, do not change PageSpeed equivalence, and do not rerun the full 99 trials yet.

1. Terra prepares a narrow, compile-gated native memory probe only.  It should record, by process and worker, startup and peak `smaps_rollup`/cgroup components, thread-stack count/size, queue/request-buffer capacities, cache/metadata mapping sizes, and socket memory; it must separate mode-off static requests from process initialization.  It must not alter behavior or produce permanent output.
2. Root shows the probe diff and a brief scope summary.  User approval is required before running it.
3. Terra reports measured components and one smallest proposed change.  Root requests user approval before applying that one change.
4. After approval, Terra applies the change, removes all probes, runs focused correctness/native surface tests, then a focused three-pass A/B on the affected Target-1 cells.  It reports RPS, cgroup, RSS, errors, and whether the expected fixed-memory component changed.
5. Only after a substantial focused gain and explicit user approval may Terra run another full native AMD64 99-trial production-scaling Target-1 rail.  The full rail remains the only way to update all eleven acceptance verdicts.

ARM is explicitly excluded from this next cycle at user direction.  No ARM result is needed or should be presented as acceptance evidence.

## Target 2 status and evidence boundary

Target 2 is paused.  The last sentence in section 23 predates this partial attempt and is superseded by this status.  The NGINX Laghu-versus-PageSpeed attempt stopped at 25 of 66 trials when the RSS sampler found a short-lived worker via `/proc` and then lost it before reading its status.  Completed HTTP checks were clean, but the partial run is invalid: it has neither a complete three-run median set nor a comparable memory result.  It yields no Target-2 RPS, memory, or PageSpeed conclusion.

No Target-2 sampler repair, rerun, or configuration change is authorized in this cycle.  The partial Target-2 bundle is not present in this checkout, so its exact path cannot be independently verified here; preserve it wherever it was generated and label it invalid if recovered.  This absence is deliberate documentation of an evidence gap, not permission to discard or replace it.

## Provenance, limitations, and audit rules

* Primary native Target-1 evidence: `tmp/benchmarks/native-amd64-target1-after-20260824/`, especially `results.json` plus its 99 per-trial JSON summaries and corpus manifest.
* Retained directional-before evidence: `tmp/benchmarks/native-amd64-target1-before-retained-20260823/`; use only with the equivalence-metadata caveat above.
* Focused retained/rejected experiment evidence: `tmp/benchmarks/local-arm64-access-log-ab-20260823/`, `tmp/benchmarks/local-arm64-target1-chunked-probe-20260823/`, `tmp/benchmarks/local-arm64-target1-chunked-fix-20260823/`, `tmp/p13-mode-off-ab/raw/`, `tmp/connection-lifecycle-probe/20260824T130500Z/`, `tmp/p7-static-cache-ab/20260824T155200Z/`, `tmp/static-cgroup-probe/20260824T165000Z/`, `tmp/route-policy-precompile/`, `tmp/p14-thinlto-ab/raw/`, `tmp/p14-pgo-ab/`, and `tmp/p15-allocator-ab/`.
* Historical `20260821T195000Z-native-pass{1,2,3}` and `20260821T195000Z-native-normalized-summary.json` were specified as immutable evidence on `linux-fast-build`, but they are not present in this checkout and their exact remote path remains unverified.  Do not infer deletion, mutation, or current parity from their absence here.
* Peak process RSS is summed per process by the rail.  For multi-process servers it may count shared pages more than once; cgroup peak is therefore the stronger total-container measure, but both metrics remain locked acceptance gates.
* Never mix ARM diagnostic A/B values with native AMD64 acceptance values, aggregate unlike cells, substitute latency for RPS, include the upstream origin in standalone memory, or use an optimization-only path to explain Target 1 without direct evidence.
* The current repository has staged benchmark/audit work outside this appended handoff.  This section changes no product source, runner, ROADMAP, `1-pager.md`, raw artifact, or prior audit text.
