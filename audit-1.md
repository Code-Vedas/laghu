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

|  Order | Change                                                      | Why first                                       |
| -----: | ----------------------------------------------------------- | ----------------------------------------------- |
|  **1** | Repair benchmark CPU/log/cache equivalence                  | Establish trustworthy baseline                  |
|  **2** | Make access logging off/async; remove `fflush` request path | Very low-risk high reward                       |
|  **3** | Stop recompressing existing gzip/Brotli artifacts           | Huge optimized-mode CPU waste                   |
|  **4** | Cache queue capability/heartbeat outside request path       | Removes `flock` + queue scan                    |
|  **5** | Implement correct incremental chunk parser                  | Fixes worker stalls + 10 MiB allocation         |
|  **6** | Build event-driven downstream connection layer + keep-alive | Fundamental RPS scalability                     |
|  **7** | Add direct static/cache artifact serving                    | RPS + allocation/RSS gain                       |
|  **8** | Split global proxy mutex / reactor-local origin pools       | Scaling after multiple reactors                 |
|  **9** | Compile routing/policies/config once                        | Strip request setup cost                        |
| **10** | Make cache/catalog metadata truly hot                       | Remove stat/open/read/checksum pressure         |
| **11** | Replace RUM mutex+linear scan                               | Optimized HTML concurrency                      |
| **12** | Merge HTML discovery/rewrite into one pass                  | Large complex-page improvement                  |
| **13** | Introduce request arenas/reusable buffers                   | Remove allocator churn                          |
| **14** | Add ThinLTO/LTO + PGO                                       | Exploit cleaner architecture                    |
| **15** | Evaluate jemalloc/mimalloc, CPU tuning                      | Only after allocation architecture is corrected |

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
