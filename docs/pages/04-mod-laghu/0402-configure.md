---
title: Configuration
parent: mod-laghu
nav_order: 2
permalink: /mod-laghu/configure/
---

# Configure mod-laghu

Apache exposes one `Laghu` directive.
Optimization settings inherit through main server, virtual host, directory, and location configuration; RUM-store settings are accepted only in main server configuration.

Laghu transaction records use Apache's configured ErrorLog sink. Each native error-log line contains one laghu_json=<JSON> payload in the stable laghu-log-v1 schema. Parse the payload, not Apache's prefix. The schema never records request queries, headers, bodies, credentials, tokens, hosts, or cache keys.

| Directive | Context | Accepted value | Default | Effect |
| --- | --- | --- | --- | --- |
| `Laghu On\|Off` | inherited | boolean | `Off` | Enables or bypasses transformation. |
| `Laghu Preset NAME` | inherited | `safe`, `balanced`, `aggressive`, `ecommerce`, `blog`, `static` | `balanced` | Selects a policy preset. |
| `Laghu RewriteLevel NAME` | inherited | `passthrough`, `bandwidth`, `core`, `all`, `experimental` | unset | Selects a rewrite level instead of a preset. |
| `Laghu EnableFilter NAME` | inherited, repeatable | filter name | none | Enables one filter after resolving the baseline policy. |
| `Laghu DisableFilter NAME` | inherited, repeatable | filter name | none | Disables one filter; a child scope may re-enable it. |
| `Laghu ForbidFilter NAME` | inherited, repeatable | filter name | none | Disables one filter permanently for this scope and descendants. |
| `Laghu AllowResources PATTERN` | inherited, repeatable to 8 | bounded URL glob | none | Restricts optimization to matching resources when any allow rule exists. |
| `Laghu Disallow PATTERN` | inherited, repeatable to 8 | bounded URL glob | none | Excludes matching resources; deny rules always win. |
| `Laghu Domain HTTPS_ORIGIN` | inherited, repeatable to 8 | exact HTTPS origin | none | Authorizes a public rewrite or shard origin. |
| `Laghu MapRewriteDomain HTTPS_PUBLIC HTTPS_SOURCE` | inherited, repeatable to 8 | exact HTTPS origins | none | Rewrites matching resource URLs from source to public. |
| `Laghu ShardDomain HTTPS_PUBLIC HTTPS_SHARDS` | inherited, up to 8 groups with 1-8 shards each | a mapped public origin and comma-separated exact HTTPS origins | none | Deterministically selects only this public origin's shard group. |
| `Laghu MapProxyDomain HTTPS_PUBLIC HTTPS_SOURCE` | inherited, repeatable to 8 | exact HTTPS origins | none | Migration-compatible public proxy mapping; rewrites source resource URLs to the public proxy origin. |
| `Laghu RespectVary On\|Off` | inherited | boolean | `On` | Bypasses unsupported `Vary` dimensions; unsafe variants are never published. |
| `Laghu RespectXForwardedProto On\|Off` | inherited | boolean | `Off` | Uses a valid forwarded scheme only from a trusted direct peer. |
| `Laghu TrustedProxy CIDR` | inherited, repeatable | IPv4/IPv6 CIDR | none | Defines direct peers trusted for forwarded scheme handling. |
| `Laghu QueryFilterOverrides On\|Off` | inherited | boolean | `Off` | Enables bounded `laghuFilters=+name,-name` request overrides. |
| `Laghu AllowApi On\|Off` | inherited | boolean | `Off` | Allows otherwise excluded API/GraphQL paths. |
| `Laghu ImageQuality N` | inherited | `1..100` | codec default | Overrides lossy image quality. |
| `Laghu ImageBeacon On\|Off` | inherited | boolean | `Off` | Enables critical-image observations. |
| `Laghu CriticalCssBeacon On\|Off` | inherited | boolean | `Off` | Enables critical-CSS learning. |
| `Laghu InstrumentationBeacon On\|Off` | inherited | boolean | `Off` | Injects bounded RUM instrumentation when CSP permits. |
| `Laghu InstrumentationSampleRate N` | inherited | `0..100` | `10` | Sets browser-side percentage sampling. |
| `Laghu OptimizationProfiles On\|Off` | inherited | boolean | `Off` | Allows a ready, template-scoped RUM profile to apply the existing safe LCP prioritization; it never enables a new rewrite. |
| `Laghu JavaScriptDeferSuggestions On\|Off` | inherited | boolean | `On` | Enables bounded RUM deferral recommendations; it never applies them. |
| `Laghu IncludeJsSourceMaps On\|Off` | inherited | boolean | `Off` | Emits immutable external SWC source maps without source content. |
| `Laghu ImageInlineLimit BYTES` | inherited | `0..16384` | `2048` | Caps image inlining. |
| `Laghu ImageMetadataLimit N` | inherited | `1..100000` | `10000` | Bounds learned image records. |
| `Laghu ImageMetadataTtl DURATION` | inherited | `1h..30d` | `7d` | Expires learned metadata. |
| `Laghu CssInlineLimit BYTES` | inherited | `0..65536` | `2048` | Caps inline and critical CSS. |
| `Laghu CssOutlineThreshold BYTES` | inherited | `1024..1048576` | `8192` | Selects large inline CSS for outlining. |
| `Laghu JavaScriptInlineLimit BYTES` | inherited | `0..65536` | `2048` | Caps external JavaScript inlining. |
| `Laghu JavaScriptOutlineThreshold BYTES` | inherited | `1024..1048576` | `8192` | Selects inline JavaScript for outlining. |
| `Laghu TransformMemoryLimit SIZE` | inherited | `4m..256m` | `32m` | Hard ceiling for temporary request-transform memory. |
| `Laghu TransformDeadlineMs N` | inherited | `5..1000` | `50` | Monotonic request-transform deadline in milliseconds. |
| `Laghu VariantsPerSource N` | inherited | `1..64` | `16` | Bounds cached variants for one canonical source. |
| `Laghu WorkerQueue PATH` | inherited | bounded path | `/run/laghu/jobs.queue` | Selects the image queue. |
| `Laghu AssetOffloadConfig PATH` | inherited | valid asset policy file | unset | Enables verified immutable CDN rewriting. |
| `Laghu AssetUploadQueue PATH` | inherited | policy-matching path | unset | Selects the asynchronous asset spool. |
| `Laghu LoadFromFile Off\|Mapped\|Native\|Both` | server/vhost | source-loader mode | `Off` | Enables asynchronous mapped or native-root acquisition. |
| `Laghu FileSourceMap SOURCE_PREFIX ROOT` | server/vhost, repeatable to 8 | normalized HTTPS prefix and absolute local root | none | Maps an eligible origin prefix to a local file tree. |
| `Laghu FontFetchQueue PATH` | inherited | bounded path | `/run/laghu/fonts.queue` | Selects the font-fetch queue. |
| `Laghu FontProviderConfig PATH` | inherited | valid provider file | unset | Enables configured external-font providers. |
| `Laghu JavaScriptQueue PATH` | inherited | bounded path | `/run/laghu/javascript.queue` | Selects the SWC queue. |
| `Laghu JavaScriptTarget QUERY` | inherited | bounded Browserslist query | `defaults and supports es6-module and not dead` | Controls syntax lowering. |
| `Laghu JavaScriptObservationConfig PATH` | inherited | valid observation file | unset | Adds exact third-party script candidates. |
| `Laghu JavaScriptDeferConfig PATH` | inherited | valid approval file | unset | Approves exact same-origin deferrals and exact third-party interaction delays. |
| `Laghu FileCacheBackend URI` | inherited | local absolute `file:` URI | `file:///var/cache/laghu/images` | Selects the cache provider and location. |
| `Laghu FileCacheSize SIZE` | inherited | at least `1m`, `k`/`m`/`g` suffix accepted | `10g` | Bounds cached payload bytes. |
| `Laghu FileCacheInodeLimit N` | inherited | `16..100000000` | `100000` | Bounds files used by payloads, canonical records, and aliases. |
| `Laghu FileCacheCleanInterval DURATION` | inherited | `1s..24h` | `60s` | Sets background approximate-LRU maintenance cadence. |
| `Laghu FileCacheMetadataSize SIZE` | inherited | `16k..1g` | `16m` | Bounds shared metadata; payload bytes are never stored there. |
| `Laghu ImageCache PATH` | inherited | bounded path | deprecated | Compatibility alias for a local file backend; conflicts with `FileCacheBackend`. |
| `Laghu PurgeMethod PURGE` | inherited | literal `PURGE` | disabled | Enables authenticated method-driven URL purge. |
| `Laghu PurgeQuery On\|Off` | inherited | boolean | `Off` | Enables authenticated `laghu=purge` query control. |
| `Laghu PurgeTokenFile PATH` | inherited | absolute protected file | unset | Loads the administrator token; never place the token in configuration. |
| `Laghu PurgeAllow CIDR` | inherited, repeatable | IPv4/IPv6 CIDR | none | Restricts administration to matching direct peers. |
| `Laghu CacheFlushFile PATH` | inherited | absolute protected file | unset | Polls `laghu-cache-flush-v1 GENERATION` full-cache invalidations. |
| `Laghu Statistics On\|Off` | inherited | boolean | `Off` | Enables authenticated `GET`/`HEAD /.laghu/stats`. |
| `Laghu Metrics On\|Off` | inherited | boolean | `Off` | Enables authenticated Prometheus text at `GET`/`HEAD /.laghu/metrics`. |
| `Laghu Readiness On\|Off` | inherited | boolean | `Off` | Enables authenticated aggregate readiness JSON at `GET`/`HEAD /.laghu/ready`. |
| `Laghu ReadinessPolicy Degraded\|Strict` | inherited | readiness policy | `Degraded` | Keeps fail-open worker loss ready but degraded, or makes required-worker loss return `503`. |
| `Laghu RumStore URI` | main server | `memory:`, `local:`, supported Redis URI | `local:` | Selects RUM persistence/synchronization. |
| `Laghu RumStoreLocalSnapshot PATH` | main server | bounded path | `<file cache>/rum.snapshot` | Selects last-known-good snapshot storage. |
| `Laghu RumStoreClientLibrary PATH` | main server | hiredis library path | unset | Enables runtime-loaded Redis/Valkey support. |
| `Laghu RumStoreTimeout MS` | main server | `10..10000` | `100` | Bounds backend operations. |
| `Laghu RumStoreTtl SECONDS` | main server | `3600..2592000` | `604800` | Sets aggregate expiry. |
| `Laghu RumStoreRetryLimit N` | main server | `0..10` | `3` | Bounds synchronization retries. |
| `Laghu RumStoreSyncInterval SECONDS` | main server | `1..300` | `5` | Sets synchronization cadence. |
| `Laghu RumStoreMemoryLimit BYTES` | main server | `16KiB..1GiB`, size suffix accepted | `64MiB` | Bounds resident RUM records. |
| `Laghu RumStorePendingLimit BYTES` | main server | `16KiB..1GiB`, size suffix accepted | `16MiB` | Bounds unsynchronized deltas. |
| `Laghu RumStoreRequired On\|Off` | main server | boolean | `Off` | Makes RUM initialization failure fatal. |

`Preset` and `RewriteLevel` cannot appear together in the same scope.
Filter names are `image_lossless`, `image_metadata`, `image_dimensions`, `image_modern`, `image_responsive`, `image_lazyload`, `html_minify`, `css_minify`, `javascript_minify`, `resource_hints`, `cache_extension`, `resource_combine`, `resource_inline`, `critical_css`, `javascript_defer`, `immutable_cache`, and `cache_media`.
Each filter may be declared only once per scope; duplicate or conflicting controls fail startup.
Resource patterns start with `/`, `http://`, or `https://`; `*` matches any sequence and `?` one character. Rules match normalized URLs without fragments, with inherited deny precedence.
Query overrides cannot bypass forbidden filters or denied resources and remain in the origin-visible query and cache identity. Encode `+` as `%2B` for form-style clients.

The file cache is the only cache backend currently implemented. Its portable shared mapping contains bounded keys, sizes, state, and approximate-LRU access epochs only; response bodies and generated assets remain on disk. Unsupported schemes such as `memcached:` fail configuration so a future provider can implement the same contract without silently changing behavior.
An inherited `DisableFilter` may be reversed with `EnableFilter`, but an inherited `ForbidFilter` cannot be reversed.
Enabling a filter with the `passthrough` rewrite level is invalid.
Provider and observation files are validated during configuration loading.
File loading requires asset offload configuration and remains capture-first and worker-only. Explicit mappings win over native `DocumentRoot` inference. Roots, every path component, and the final regular file are checked; symlinks, traversal, reparse points, device paths, and changed files fail open.
Remote synchronization requires verified `rediss://`; `redis://` is restricted to loopback development, and Memcached is unsupported.

```apache
Laghu RumStore local:
Laghu RumStoreLocalSnapshot /var/lib/laghu/rum/rum.snapshot

<VirtualHost *:80>
  Laghu On
  Laghu Preset balanced
</VirtualHost>
```

## JavaScript deferral, interaction approvals, and task yields

The approval file accepts exact, root-relative, query-free same-origin deferrals and exact HTTPS third-party interaction delays, each optionally scoped to one exact template:

```text
defer /assets/analytics.js
defer /assets/checkout.js template=/checkout/
interaction https://cdn.example.test/analytics.js template=/checkout/
```

Interaction URLs must be default-port HTTPS URLs without credentials, query, or fragment, and must exactly match a third-party script already present in the response. Laghu delays only a classic external script with no body and no attributes except `src` and an optional safe nonce. The defer filter and CSP must permit both the original external script and Laghu's nonce-bearing loader; otherwise markup passes through unchanged. First `pointerdown`, `keydown`, or `touchstart` loads the approved script in document order. Instrumentation and the defer filter must both be enabled. Deferral suggestions require 100 fresh bucket observations, at least 90 percent script coverage, current SWC safety metadata, and safe ordering. Run the Apache configuration test and reload after editing the file. Approval never bypasses current evidence; rollback notices require 50 post-enable observations, and removing the line plus reloading performs the rollback.

To split an application-owned long task, mark its inline script `<script data-laghu-yield="cooperative" nonce="…">` and explicitly `await window.Laghu.yield()` between independent chunks. With the defer filter enabled, Laghu inserts a nonce-preserving helper before that script. The helper prefers `scheduler.postTask`, then `requestIdleCallback`, then `setTimeout`, and preserves an existing `Laghu.yield`. Missing or invalid marker, missing/invalid nonce, or CSP denial leaves markup unchanged.
