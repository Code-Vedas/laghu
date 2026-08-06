---
title: Configuration
parent: Laghu Server
nav_order: 2
permalink: /laghu-server/configure/
---

# Configure the Laghu Server

The standalone server accepts command-line options only.
`--listen`, `--origin`, one of `--file-cache-backend` or deprecated `--cache`, and `--worker-queue` are required; duplicate, conflicting, or unknown options fail startup.

| Option | Accepted value | Default | Effect |
| --- | --- | --- | --- |
| `--listen HOST:PORT` | valid endpoint | required | Selects the client listener. |
| `--origin http[s]://HOST[:PORT]` | one origin without path/query/credentials | required | Selects the upstream origin. |
| `--file-cache-backend URI` | local absolute `file:` URI | required | Selects the cache provider and location. |
| `--file-cache-size SIZE` | at least `1m`, `k`/`m`/`g` suffix accepted | `10g` | Bounds cached payload bytes. |
| `--file-cache-inode-limit N` | `16..100000000` | `100000` | Bounds backend files. |
| `--file-cache-clean-interval DURATION` | `1s..24h` | `60s` | Sets background approximate-LRU maintenance cadence. |
| `--file-cache-metadata-size SIZE` | `16k..1g` | `16m` | Bounds shared metadata; it never stores payload bytes. |
| `--transform-memory-limit SIZE` | `4m..256m` | `32m` | Hard ceiling for temporary request-transform memory. |
| `--transform-deadline-ms N` | `5..1000` | `50` | Monotonic request-transform deadline in milliseconds. |
| `--variants-per-source N` | `1..64` | `16` | Bounds cached variants for one canonical source. |
| `--cache PATH` | bounded path | deprecated | Compatibility alias for a local file backend. |
| `--worker-queue PATH` | bounded path | required | Selects the image queue. |
| `--cache-mime-types LIST` | comma-separated MIME types | empty | Explicitly enables opaque media cache extension for matching response types. |
| `--preset NAME` | supported preset | `balanced` | Selects policy; conflicts with `--rewrite-level`. |
| `--rewrite-level NAME` | supported rewrite level | unset | Selects policy; conflicts with `--preset`. |
| `--enable-filter NAME` | filter name, repeatable | none | Enables one filter after resolving the baseline policy. |
| `--disable-filter NAME` | filter name, repeatable | none | Disables one filter. |
| `--forbid-filter NAME` | filter name, repeatable | none | Permanently disables one filter for configuration parity. |
| `--allow-resources PATTERN` | bounded URL glob, repeatable to 8 | none | Restricts optimization to matching resources when configured. |
| `--disallow PATTERN` | bounded URL glob, repeatable to 8 | none | Excludes matching resources; deny rules win. |
| `--domain HTTPS_ORIGIN` | exact HTTPS origin, repeatable to 8 | none | Authorizes a public rewrite or shard origin. |
| `--map-rewrite-domain HTTPS_PUBLIC HTTPS_SOURCE` | exact HTTPS origins, repeatable to 8 | none | Rewrites matching resource URLs from source to public. |
| `--shard-domain HTTPS_PUBLIC HTTPS_SHARDS` | a mapped public origin and comma-separated exact HTTPS origins, up to 8 groups with 1-8 shards each | none | Deterministically selects only this public origin's shard group. |
| `--map-proxy-domain HTTPS_PUBLIC HTTPS_SOURCE` | exact HTTPS origins, repeatable to 8 | none | Migration-compatible public proxy mapping; rewrites source resource URLs to the public proxy origin. |
| `--respect-vary on\|off` | boolean | `on` | Bypasses unsupported response variation dimensions. |
| `--respect-x-forwarded-proto on\|off` | boolean | `off` | Uses a valid forwarded scheme only from a trusted direct peer. |
| `--query-filter-overrides on\|off` | boolean | `off` | Enables bounded `laghuFilters=+name,-name` request overrides. |
| `--allow-api` | flag | off | Allows otherwise excluded API/GraphQL paths. |
| `--image-quality N` | `1..100` | codec default | Overrides lossy image quality. |
| `--image-beacon` | flag | off | Enables critical-image observations. |
| `--critical-css-beacon` | flag | off | Enables critical-CSS learning. |
| `--instrumentation-beacon` | flag | off | Enables RUM script injection when CSP permits. |
| `--instrumentation-sample-rate N` | `0..100` | `10` | Sets browser-side percentage sampling. |
| `--javascript-defer-suggestions on\|off` | boolean | `on` | Enables bounded RUM deferral recommendations; it never applies them. |
| `--include-js-source-maps` | flag | off | Emits immutable external SWC source maps without source content. |
| `--font-fetch-queue PATH` | bounded path | unset | Enables the font-fetch queue; requires provider config. |
| `--font-provider-config PATH` | valid provider file | unset | Enables providers; requires font queue. |
| `--javascript-queue PATH` | bounded path | unset | Enables the SWC queue. |
| `--asset-offload-config PATH` | valid asset policy file | unset | Enables verified immutable CDN rewriting. |
| `--asset-upload-queue PATH` | policy-matching path | unset | Selects the asynchronous asset spool. |
| `--load-from-file off\|mapped` | source-loader mode | `off` | Enables asynchronous explicitly mapped file acquisition. |
| `--file-source-map SOURCE_PREFIX=ROOT` | normalized HTTPS prefix and absolute local root, repeatable to 8 | none | Maps an eligible origin prefix to a local file tree. |
| `--javascript-target QUERY` | bounded Browserslist query | `defaults and supports es6-module and not dead` | Controls syntax lowering. |
| `--javascript-inline-limit N` | `0..65536` | `2048` | Caps JavaScript inlining. |
| `--javascript-outline-threshold N` | `1024..1048576` | `8192` | Selects inline scripts for outlining. |
| `--javascript-observation-config PATH` | valid observation file | unset | Adds exact third-party script candidates. |
| `--javascript-defer-config PATH` | valid approval file | unset | Approves exact same-origin scripts for evidence-gated deferral. |
| `--workers N` | `1..256` | `4` | Sets request worker threads. |
| `--connection-queue N` | `1..65536` | `64` | Bounds accepted connections waiting for workers. |
| `--connect-timeout SECONDS` | `1..300` | `5` | Bounds origin connection establishment. |
| `--io-timeout SECONDS` | `1..300` | `30` | Bounds client/origin I/O. |
| `--drain-timeout SECONDS` | `1..300` | `30` | Bounds graceful shutdown. |
| `--origin-ca-file PATH` | CA bundle path | platform trust | Overrides trust for HTTPS origins; invalid with HTTP. |
| `--forwarded-headers MODE` | `off`, `forwarded`, `x-forwarded`, `both` | `off` | Selects trusted forwarding syntax. |
| `--trusted-proxy CIDR` | canonical IPv4/IPv6 CIDR, repeatable to 64 | none | Trusts forwarding headers from matching peers; requires forwarding mode. |
| `--purge-method PURGE` | literal `PURGE` | disabled | Enables authenticated method-driven URL purge. |
| `--purge-query on\|off` | boolean | `off` | Enables authenticated `laghu=purge` query control. |
| `--purge-token-file PATH` | absolute protected file | unset | Loads the administrator token; never pass the token as an argument. |
| `--purge-allow CIDR` | canonical IPv4/IPv6 CIDR, repeatable to 64 | none | Restricts administration to matching direct peers. |
| `--cache-flush-file PATH` | absolute protected file | unset | Polls `laghu-cache-flush-v1 GENERATION` full-cache invalidations. |
| `--statistics on\|off` | boolean | `off` | Enables authenticated `GET`/`HEAD /.laghu/stats`. |
| `--metrics on\|off` | boolean | `off` | Enables authenticated Prometheus text at `GET`/`HEAD /.laghu/metrics`. |
| `--readiness on\|off` | boolean | `off` | Enables authenticated aggregate readiness JSON at `GET`/`HEAD /.laghu/ready`. |
| `--readiness-policy degraded\|strict` | readiness policy | `degraded` | Keeps fail-open worker loss ready but degraded, or makes required-worker loss return `503`. |
| `--rum-store URI` | `memory:`, `local:`, supported Redis URI | `local:` | Selects RUM persistence/synchronization. |
| `--rum-store-local-snapshot PATH` | bounded path | `<cache>/rum.snapshot` | Selects last-known-good snapshot storage. |
| `--rum-store-client-library PATH` | hiredis library path | unset | Enables Redis/Valkey support. |
| `--rum-store-timeout MS` | `10..10000` | `100` | Bounds backend operations. |
| `--rum-store-ttl SECONDS` | `3600..2592000` | `604800` | Sets aggregate expiry. |
| `--rum-store-retry-limit N` | `0..10` | `3` | Bounds synchronization retries. |
| `--rum-store-sync-interval SECONDS` | `1..300` | `5` | Sets synchronization cadence. |
| `--rum-store-memory-limit BYTES` | `16KiB..1GiB`, size suffix accepted | `64MiB` | Bounds resident RUM records. |
| `--rum-store-pending-limit BYTES` | `16KiB..1GiB`, size suffix accepted | `16MiB` | Bounds pending deltas. |
| `--rum-store-required` | flag | off | Makes RUM initialization failure fatal. |
| `--help` | flag | n/a | Prints usage. |
| `--version` | flag | n/a | Prints version. |

The proxy itself defaults enabled with `balanced`, unlike the disabled-by-default native modules.
Filter names are `image_lossless`, `image_metadata`, `image_dimensions`, `image_modern`, `image_responsive`, `image_lazyload`, `html_minify`, `css_minify`, `javascript_minify`, `resource_hints`, `cache_extension`, `resource_combine`, `resource_inline`, `critical_css`, `javascript_defer`, `immutable_cache`, and `cache_media`.
Each filter may be controlled only once; duplicates and conflicts fail startup, as does enabling a filter with `passthrough`.
Resource patterns begin with `/`, `http://`, or `https://`; support `*` and `?`; and match normalized URLs without fragments. Query overrides cannot bypass forbidden filters or denied resources, remain origin-visible, and participate in cache identity. Encode `+` as `%2B` for form-style clients.

The file cache is the only cache backend currently implemented. Its portable shared mapping contains bounded keys, sizes, state, and approximate-LRU access epochs only; response bodies and generated assets remain on disk. Unsupported schemes such as `memcached:` fail startup so a future provider can implement the same contract without changing callers.
Remote RUM synchronization requires verified `rediss://`; `redis://` is loopback-only and Memcached is unsupported.
Mapped loading requires asset offload configuration and remains capture-first and worker-only. Standalone rejects `native` and `both` because its origin is remote. Roots, every path component, and the final regular file are checked; symlinks, traversal, reparse points, device paths, and changed files fail open.

## JavaScript deferral approvals

The approval file accepts exact, root-relative, query-free script paths, optionally scoped to one exact template:

```text
defer /assets/analytics.js
defer /assets/checkout.js template=/checkout/
```

Instrumentation and the defer filter must both be enabled. Suggestions require 100 fresh bucket observations, at least 90 percent script coverage, current SWC safety metadata, and safe ordering. Reload the service after editing the file. Approval never bypasses current evidence; rollback notices require 50 post-enable observations, and removing the line plus reloading performs the rollback.
