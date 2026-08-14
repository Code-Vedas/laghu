---
title: Configuration
parent: ngx-laghu
nav_order: 2
permalink: /ngx-laghu/configure/
---

# Configure ngx-laghu

NGINX exposes one `laghu` directive with a setting and value.
Most settings are inherited through `http`, `server`, and `location`; RUM-store settings are process-wide and valid only in `http`.

Laghu transaction records use the configured NGINX error_log sink. Each native error-log line contains one laghu_json=<JSON> payload in the stable laghu-log-v1 schema. Parse the payload, not the NGINX prefix. The schema never records request queries, headers, bodies, credentials, tokens, hosts, or cache keys.

| Directive | Context | Accepted value | Default | Effect |
| --- | --- | --- | --- | --- |
| `laghu on\|off;` | inherited | boolean | `off` | Enables or bypasses transformation. |
| `laghu preset NAME;` | inherited | `safe`, `balanced`, `aggressive`, `ecommerce`, `blog`, `static` | `balanced` | Selects a policy preset. |
| `laghu rewrite_level NAME;` | inherited | `passthrough`, `bandwidth`, `core`, `all`, `experimental` | unset | Selects a rewrite level instead of a preset. |
| `laghu enable_filter NAME;` | inherited, repeatable | filter name | none | Enables one filter after resolving the baseline policy. |
| `laghu disable_filter NAME;` | inherited, repeatable | filter name | none | Disables one filter; a child scope may re-enable it. |
| `laghu forbid_filter NAME;` | inherited, repeatable | filter name | none | Disables one filter permanently for this scope and descendants. |
| `laghu allow_resources PATTERN;` | inherited, repeatable to 8 | bounded URL glob | none | Restricts optimization to matching resources when any allow rule exists. |
| `laghu disallow PATTERN;` | inherited, repeatable to 8 | bounded URL glob | none | Excludes matching resources; deny rules always win. |
| `laghu domain HTTPS_ORIGIN;` | inherited, repeatable to 8 | exact HTTPS origin | none | Authorizes a public rewrite or shard origin. |
| `laghu map_rewrite_domain HTTPS_PUBLIC HTTPS_SOURCE;` | inherited, repeatable to 8 | exact HTTPS origins | none | Rewrites matching resource URLs from source to public. |
| `laghu shard_domain HTTPS_PUBLIC HTTPS_SHARDS;` | inherited, up to 8 groups with 1-8 shards each | a mapped public origin and comma-separated exact HTTPS origins | none | Deterministically selects only this public origin's shard group. |
| `laghu map_proxy_domain HTTPS_PUBLIC HTTPS_SOURCE;` | inherited, repeatable to 8 | exact HTTPS origins | none | Migration-compatible public proxy mapping; rewrites source resource URLs to the public proxy origin. |
| `laghu respect_vary on\|off;` | inherited | boolean | `on` | Bypasses unsupported `Vary` dimensions; unsafe variants are never published. |
| `laghu respect_x_forwarded_proto on\|off;` | inherited | boolean | `off` | Uses a valid forwarded scheme only from a trusted direct peer. |
| `laghu trusted_proxy CIDR;` | inherited, repeatable | IPv4/IPv6 CIDR | none | Defines direct peers trusted for forwarded scheme handling. |
| `laghu query_filter_overrides on\|off;` | inherited | boolean | `off` | Enables bounded `laghuFilters=+name,-name` request overrides. |
| `laghu allow_api on\|off;` | inherited | boolean | `off` | Allows otherwise excluded API/GraphQL paths. |
| `laghu image_quality N;` | inherited | `1..100` | codec default | Overrides lossy image quality. |
| `laghu image_beacon on\|off;` | inherited | boolean | `off` | Enables critical-image observations. |
| `laghu critical_css_beacon on\|off;` | inherited | boolean | `off` | Enables critical-CSS learning. |
| `laghu instrumentation_beacon on\|off;` | inherited | boolean | `off` | Injects bounded RUM instrumentation when CSP permits. |
| `laghu instrumentation_sample_rate N;` | inherited | `0..100` | `10` | Sets browser-side percentage sampling. |
| `laghu optimization_profiles on\|off;` | inherited | boolean | `off` | Permits ready, template-scoped RUM actions: LCP prioritization and safe CLS dimension reservation. |
| `laghu javascript_defer_suggestions on\|off;` | inherited | boolean | `on` | Enables bounded RUM deferral recommendations; it never applies them. |
| `laghu include_js_source_maps on\|off;` | inherited | boolean | `off` | Emits immutable external SWC source maps without source content. |
| `laghu image_inline_limit BYTES;` | inherited | `0..16384` | `2048` | Caps image data-URI inlining; zero disables it. |
| `laghu image_metadata_limit N;` | inherited | `1..100000` | `10000` | Bounds learned image records. |
| `laghu image_metadata_ttl DURATION;` | inherited | `1h..30d` | `7d` | Expires learned metadata. |
| `laghu css_inline_limit BYTES;` | inherited | `0..65536` | `2048` | Caps inline CSS and critical CSS. |
| `laghu css_outline_threshold BYTES;` | inherited | `1024..1048576` | `8192` | Selects large inline CSS for outlining. |
| `laghu javascript_inline_limit BYTES;` | inherited | `0..65536` | `2048` | Caps external JavaScript inlining. |
| `laghu javascript_outline_threshold BYTES;` | inherited | `1024..1048576` | `8192` | Selects inline JavaScript for outlining. |
| `laghu transform_memory_limit SIZE;` | inherited | `4m..256m` | `32m` | Hard ceiling for temporary request-transform memory. |
| `laghu transform_deadline_ms N;` | inherited | `5..1000` | `50` | Monotonic request-transform deadline in milliseconds. |
| `laghu variants_per_source N;` | inherited | `1..64` | `16` | Bounds cached variants for one canonical source. |
| `laghu worker_queue PATH;` | inherited | bounded path | `/run/laghu/jobs.queue` | Selects the image queue. |
| `laghu asset_offload_config PATH;` | inherited | valid asset policy file | unset | Enables verified immutable CDN rewriting. |
| `laghu asset_upload_queue PATH;` | inherited | policy-matching path | unset | Selects the asynchronous asset spool. |
| `laghu load_from_file off\|mapped\|native\|both;` | inherited | source-loader mode | `off` | Enables asynchronous mapped or native-root acquisition. |
| `laghu file_source_map SOURCE_PREFIX ROOT;` | inherited, repeatable to 8 | normalized HTTPS prefix and absolute local root | none | Maps an eligible origin prefix to a local file tree. |
| `laghu font_fetch_queue PATH;` | inherited | bounded path | `/run/laghu/fonts.queue` | Selects the external-font queue. |
| `laghu font_provider_config PATH;` | inherited | valid provider file | unset | Enables configured external-font providers. |
| `laghu javascript_queue PATH;` | inherited | bounded path | `/run/laghu/javascript.queue` | Selects the SWC queue. |
| `laghu chrome_analysis_queue PATH;` | inherited | bounded path | unset | Enables asynchronous optional headless-Chrome analysis; requires an operator-installed Chromium executable and running `laghu-chrome-analyze`; an unset queue performs no browser work. |
| `laghu chrome_analysis_output PATH;` | inherited | bounded directory | unset | Opts into bounded lifecycle import of one-shot Chrome reports from this directory; requires `chrome_analysis_queue`. |
| `laghu layout_reservation_config PATH;` | inherited | bounded rule file | unset | Loads exact-ID `box ID WIDTH HEIGHT` and `font ID ADJUST_MILLI` CLS reservations. Rules apply only to learned CLS regressions and CSP-permitted style attributes. |
| `laghu otel_endpoint HTTPS_URL;` | inherited | HTTPS collector URL | unset | Enables OTLP/HTTP JSON export only with `otel_trace_queue`; authorization is read only from `LAGHU_OTEL_AUTHORIZATION`. |
| `laghu otel_trace_queue PATH;` | inherited | bounded path | unset | Dedicated bounded async trace queue, consumed by `laghu-otel-export`. |
| `laghu otel_sampling_rate 0..100;` | inherited | percentage | `0` | Sampling is disabled by default; queue saturation drops spans without affecting responses. |
| `laghu otel_ca_file PATH;` | inherited | PEM trust bundle | system trust | Optional collector trust bundle. |
| `laghu chrome_analysis_timeout MS;` | inherited | `100..10000` | `1500` | Bounds one analysis job; requires `chrome_analysis_queue`. |
| `laghu javascript_target QUERY;` | inherited | bounded Browserslist query | `defaults and supports es6-module and not dead` | Controls syntax lowering without polyfills. |
| `laghu javascript_observation_config PATH;` | inherited | valid observation file | unset | Adds exact third-party script candidates. |
| `laghu javascript_defer_config PATH;` | inherited | valid approval file | unset | Approves exact same-origin deferrals and exact third-party interaction delays. |
| `laghu file_cache_backend URI;` | inherited | local absolute `file:` URI | `file:///var/cache/laghu/images` | Selects the cache provider and location. |
| `laghu file_cache_size SIZE;` | inherited | at least `1m`, size suffix accepted | `10g` | Bounds cached payload bytes. |
| `laghu file_cache_inode_limit N;` | inherited | `16..100000000` | `100000` | Bounds files used by payloads, canonical records, and aliases. |
| `laghu file_cache_clean_interval DURATION;` | inherited | `1s..24h` | `60s` | Sets background approximate-LRU maintenance cadence. |
| `laghu file_cache_metadata_size SIZE;` | inherited | `16k..1g` | `16m` | Bounds shared metadata; payload bytes are never stored there. |
| `laghu image_cache PATH;` | inherited | bounded path | deprecated | Compatibility alias for a local file backend; conflicts with `file_cache_backend`. |
| `laghu purge_method PURGE;` | inherited | literal `PURGE` | disabled | Enables authenticated method-driven URL purge. |
| `laghu purge_query on\|off;` | inherited | boolean | `off` | Enables authenticated `laghu=purge` query control. |
| `laghu purge_token_file PATH;` | inherited | absolute protected file | unset | Loads the administrator token; never place the token in configuration. |
| `laghu purge_allow CIDR;` | inherited, repeatable | IPv4/IPv6 CIDR | none | Restricts administration to matching direct peers. |
| `laghu cache_flush_file PATH;` | inherited | absolute protected file | unset | Polls `laghu-cache-flush-v1 GENERATION` full-cache invalidations. |
| `laghu statistics on\|off;` | inherited | boolean | `off` | Enables authenticated `GET`/`HEAD /.laghu/stats`. |
| `laghu metrics on\|off;` | inherited | boolean | `off` | Enables authenticated Prometheus text at `GET`/`HEAD /.laghu/metrics`. |
| `laghu readiness on\|off;` | inherited | boolean | `off` | Enables authenticated aggregate readiness JSON at `GET`/`HEAD /.laghu/ready`. |
| `laghu readiness_policy degraded\|strict;` | inherited | readiness policy | `degraded` | Keeps fail-open worker loss ready but degraded, or makes required-worker loss return `503`. |
| `laghu rum_store URI;` | `http` | `memory:`, `local:`, supported Redis URI | `local:` | Selects RUM persistence/synchronization. |
| `laghu rum_store_local_snapshot PATH;` | `http` | bounded path | `<file cache>/rum.snapshot` | Selects last-known-good snapshot storage. |
| `laghu rum_store_client_library PATH;` | `http` | hiredis library path | unset | Enables runtime-loaded Redis/Valkey support. |
| `laghu rum_store_timeout MS;` | `http` | `10..10000` | `100` | Bounds backend operations. |
| `laghu rum_store_ttl SECONDS;` | `http` | `3600..2592000` | `604800` | Sets aggregate expiry. |
| `laghu rum_store_retry_limit N;` | `http` | `0..10` | `3` | Bounds synchronization retries. |
| `laghu rum_store_sync_interval SECONDS;` | `http` | `1..300` | `5` | Sets background synchronization cadence. |
| `laghu rum_store_memory_limit BYTES;` | `http` | `16KiB..1GiB`, size suffix accepted | `64MiB` | Bounds resident RUM records. |
| `laghu rum_store_pending_limit BYTES;` | `http` | `16KiB..1GiB`, size suffix accepted | `16MiB` | Bounds unsynchronized deltas. |
| `laghu rum_store_required on\|off;` | `http` | boolean | `off` | Makes RUM initialization failure fatal. |

## Preset behavior contract

Preset selection resolves to the following tested filter families and safety
settings in the shared policy engine.

| Preset | Enabled filters | Risk | Lossy | Structural rewrite | Resource inlining | Script reordering | Image quality |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `safe` | `image_lossless`, `image_metadata`, `image_dimensions` | conservative | no | no | no | no | codec default |
| `balanced` | safe + `image_modern`, `image_responsive`, `image_lazyload`, `html_minify`, `css_minify`, `javascript_minify`, `resource_hints`, `cache_extension` | moderate | yes | yes | no | no | `82` |
| `aggressive` | balanced + `resource_combine`, `resource_inline`, `critical_css`, `javascript_defer` | expansive | yes | yes | yes | yes | `75` |
| `ecommerce` | safe + `image_modern`, `image_responsive`, `image_lazyload`, `css_minify`, `resource_hints`, `cache_extension` | conservative | yes | yes | no | no | `85` |
| `blog` | balanced + `resource_inline`, `critical_css`, `javascript_defer` | moderate | yes | yes | yes | yes | `82` |
| `static` | aggressive + `immutable_cache` | expansive | yes | yes | yes | yes | `75` |

You can always narrow a preset with `disable_filter`/`forbid_filter`, but not
with unknown tokens; unsupported values are rejected by config loaders.

`preset` and `rewrite_level` are mutually exclusive in one scope.
Filter names are `image_lossless`, `image_metadata`, `image_dimensions`, `image_modern`, `image_responsive`, `image_lazyload`, `html_minify`, `css_minify`, `javascript_minify`, `resource_hints`, `cache_extension`, `resource_combine`, `resource_inline`, `critical_css`, `javascript_defer`, `immutable_cache`, and `cache_media`.
Each filter may be declared only once per scope; duplicate or conflicting controls fail startup.
Resource patterns start with `/`, `http://`, or `https://`; `*` matches any sequence and `?` one character. Rules match the normalized URL without its fragment, and inherited duplicates, conflicts, or limit overflow fail startup.
Query overrides never bypass `forbid_filter` or resource-deny rules and remain part of the application-visible query and cache identity. In URLs, encode `+` as `%2B` when the client treats plus as a form-space.

The file cache is the only cache backend currently implemented. Its portable shared mapping contains bounded keys, sizes, state, and approximate-LRU access epochs only; response bodies and generated assets remain on disk. Unsupported schemes such as `memcached:` fail configuration so a future provider can implement the same contract without silently changing behavior.
An inherited `disable_filter` may be reversed with `enable_filter`, but an inherited `forbid_filter` cannot be reversed.
Enabling a filter with the `passthrough` rewrite level is invalid.
The provider and observation files are validated during configuration loading; malformed files fail `nginx -t`.
File loading requires asset offload configuration and remains capture-first and worker-only. Explicit mappings win over native document-root inference. Roots, every path component, and the final regular file are checked; symlinks, traversal, reparse points, device paths, and changed files fail open.
The installed package may explicitly set smaller RUM memory limits than runtime defaults.

Redis uses `redis://` only for loopback development and verified `rediss://` for remote endpoints.
Redis/Valkey synchronization is background-only and atomically merges bounded batches through a versioned Lua contract; request processing reads memory only.
Memcached is unsupported.

```nginx
http {
  laghu rum_store local:;
  laghu rum_store_local_snapshot /var/lib/laghu/rum/rum.snapshot;

  server {
    laghu on;
    laghu preset balanced;
  }
}
```

## JavaScript deferral, interaction approvals, and task yields

The approval file accepts exact, root-relative, query-free same-origin deferrals and exact HTTPS third-party interaction delays, each optionally scoped to one exact template:

```text
defer /assets/analytics.js
defer /assets/checkout.js template=/checkout/
interaction https://cdn.example.test/analytics.js template=/checkout/
```

Interaction URLs must be default-port HTTPS URLs without credentials, query, or fragment, and must exactly match a third-party script already present in the response. Laghu delays only a classic external script with no body and no attributes except `src` and an optional safe nonce. The defer filter and CSP must permit both the original external script and Laghu's nonce-bearing loader; otherwise markup passes through unchanged. First `pointerdown`, `keydown`, or `touchstart` loads the approved script in document order. Instrumentation and the defer filter must both be enabled. Deferral suggestions require 100 fresh bucket observations, at least 90 percent script coverage, current SWC safety metadata, and safe ordering. Reload NGINX after editing the file. Approval never bypasses current evidence; rollback notices require 50 post-enable observations, and removing the line plus reloading performs the rollback.

To split an application-owned long task, mark its inline script `<script data-laghu-yield="cooperative" nonce="…">` and explicitly `await window.Laghu.yield()` between independent chunks. With the defer filter enabled, Laghu inserts a nonce-preserving helper before that script. The helper prefers `scheduler.postTask`, then `requestIdleCallback`, then `setTimeout`, and preserves an existing `Laghu.yield`. Missing or invalid marker, missing/invalid nonce, or CSP denial leaves markup unchanged.
