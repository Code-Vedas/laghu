---
title: Configuration
parent: Laghu Server
nav_order: 2
permalink: /laghu-server/configure/
---

# Configure the Laghu Server

The standalone server accepts runtime settings only from strict YAML. Like NGINX, `laghu` reads its fixed main
configuration, `/etc/laghu/laghu.yaml`, then `/etc/laghu/conf.d/*.yaml` in lexical order. `--config PATH` is only an
administrative override for an alternate main configuration; it uses `PATH` and its sibling `conf.d/*.yaml` directory.
Every file requires `schema: 1` and one `runtime:` mapping. Unknown keys, duplicate top-level keys, unsupported YAML shapes,
unsafe paths, and invalid option combinations fail startup. Configuration files must be absolute regular files and cannot be
group- or world-writable.

Runtime keys are the former option name without the `--` prefix, with hyphens written as underscores. Repeated settings use a
YAML sequence. Operational commands (`status`, `doctor`, `bench`, `migrate`, `purge`, and `reload`) remain argument-driven.

```yaml
schema: 1
runtime:
  listen: 127.0.0.1:8080
  origin: http://127.0.0.1:8000
  file_cache_backend: file:///var/cache/laghu/images
  worker_queue: /run/laghu/jobs.queue
  pid_file: /run/laghu/laghu.pid
  preset: balanced
  forwarded_headers: both
  trusted_proxy:
    - 127.0.0.0/8
```

`sites` selects a host-specific static document root. `routes` are ordered and use `exact`, `prefix`, or `ordered_regex` matching;
each route declares either a local `redirect` or a `proxy_pass`. Route targets are `http://`, verified `https://`, `fastcgi://`,
`uwsgi://`, or `scgi://`; targets cannot contain a path, query, userinfo, or fragment. Optional `response_header_name` and
`response_header_value` add one validated response header to a redirect. A site can contain its own `routes` list, which only
matches that site's `Host` value. Global routes apply to every host.

Place shared Laghu policy keys in a `laghu:` mapping and shared service-setting keys in a `service:` mapping at the site or route.
Each scope is parsed by the same shared setting parsers as NGINX and Apache, then resolved once while loading: global → site →
matching route. A request borrows those immutable resolved settings; it never loads files or merges configuration. This is useful
for policy boundaries such as a conservative host or a route with `mode: off`:

```yaml
sites:
  - host: shop.example.test
    document_root: /srv/shop/public
    laghu:
      preset: safe
    service:
      javascript_target: defaults
    routes:
      - match: exact
        pattern: /checkout/legacy
        redirect: /checkout/
        laghu:
          mode: off
```

Lifecycle-owned service resources (cache backend, queues, TLS contexts, RUM engine, and source registry) remain stable for a
running process. Keep their topology global; scoped values are validated and resolved, and request behavior receives the matching
resolved service settings.

## Static and edge policy

The following keys are accepted in `runtime:` for global policy and inside a `laghu:` mapping at site or route scope. They resolve
once as global → site → route, with a child scalar replacing its parent. `allow` and `deny` are replacement lists; a matching deny
always wins. A child may change either rate field and inherits the other field from its parent.

| Key | Accepted value | Effect |
| --- | --- | --- |
| `spa_fallback` | absolute local URL path | Serves this static path when a static lookup misses; it cannot contain `..`, a query, or a fragment. |
| `request_header_limit` | `1k..64k` | Bounds complete HTTP request headers before body reads. |
| `request_body_limit` | `0..64m` | Bounds declared request bodies before body reads. |
| `static_max_bytes` | `1..64m` | Bounds a regular static file before it is read or transformed. |
| `compression` | `off` or `gzip` | Enables gzip only for eligible full static responses when the client advertises `gzip`; ranges remain byte-exact. |
| `allow`, `deny` | CIDR sequence | Enforces direct-peer IPv4 or IPv6 access policy. |
| `basic_auth_file` | absolute owner-only regular file | Loads Basic-auth records at configuration load or reload. |
| `basic_auth_realm` | printable text without `"`, CR, or LF | Sets the `401` Basic realm; requires an inherited or local auth file. |
| `rate_limit`, `rate_burst` | `1..100000` | Per direct-peer, per-resolved-scope token bucket in requests/second and maximum burst. |

An auth file is not read on a request path. It must be owned by the Laghu service account, must not grant any group or other
permission, and contains one `username:sha256:64-lower-or-upper-hex-digest` record per line. User names cannot contain whitespace
or `:`. Laghu compares the SHA-256 password digest in constant time and never logs the authorization header, password, or digest.

Every successful static response—including SPA fallback, byte ranges, and directory listings—enters the shared Laghu transaction
contract. A partial response remains partial and is not recompressed or substituted from cache. Limit, CIDR, auth, and rate
rejections are generated before origin or static body I/O.

## Upstream health and failover

An HTTP route can supply `health_check: /absolute-path` and an optional `health_interval: 1..3600` (default `5`). Lifecycle
maintenance performs bounded `GET` probes for every primary and failover target; only `2xx` and `3xx` responses are healthy.
Connection, TLS, write, framing, and pre-header read failures also mark a selected target unhealthy immediately. A later successful
active probe returns it to service.

Routes try each configured target at most once. If a `GET`, `HEAD`, or `OPTIONS` request loses its selected target before response
headers, Laghu can retry the next healthy target. Requests with other methods—including `POST`—are never replayed. FastCGI, uWSGI,
and SCGI use their native bounded request framing and CGI-style response headers. Active `health_check` applies to HTTP and HTTPS
routes; gateway protocols use the same idempotent-method failover rule, and only after bounded connection or protocol failures.
Credentials are not supported in any upstream target.

Laghu Server writes laghu-log-v1 JSON records to stderr for transactions and lifecycle changes. Transaction paths exclude the complete query string; headers, bodies, credentials, tokens, hosts, and cache keys are never emitted. Transaction records also contain bounded route kind, upstream authority, upstream protocol, access outcome, failover count, and static/SPA/compression/rate flags. They are fixed metadata, not copied request headers. Worker services use the same schema on stderr for lifecycle and job events.

| YAML key (former option) | Accepted value | Default | Effect |
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
| `--rewrite-level NAME` | supported rewrite level | unset | Selects policy; `experimental` permits negotiated JPEG XL and conflicts with `--preset`. |
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
| `--image-quality N` | `1..100` | codec default | Upper bound; adaptive presets and `Save-Data: on` may select lower quality. |
| `--image-beacon` | flag | off | Enables critical-image observations. |
| `--critical-css-beacon` | flag | off | Enables critical-CSS learning. |
| `--instrumentation-beacon` | flag | off | Enables RUM script injection when CSP permits. |
| `--instrumentation-sample-rate N` | `0..100` | `10` | Sets browser-side percentage sampling. |
| `--optimization-profiles on\|off` | boolean | `off` | Permits ready, template-scoped RUM actions: LCP prioritization and safe CLS dimension reservation. |
| `--javascript-defer-suggestions on\|off` | boolean | `on` | Enables bounded RUM deferral recommendations; it never applies them. |
| `--include-js-source-maps` | flag | off | Emits immutable external SWC source maps without source content. |
| `--font-fetch-queue PATH` | bounded path | unset | Enables the font-fetch queue; requires provider config. |
| `--font-provider-config PATH` | valid provider file | unset | Enables providers; requires font queue. |
| `--javascript-queue PATH` | bounded path | unset | Enables the SWC queue. |
| `--chrome-analysis-queue PATH` | bounded path | unset | Enables asynchronous optional headless-Chrome analysis; requires an operator-installed Chromium executable and running `laghu-chrome-analyze`; an unset queue performs no browser work. |
| `--chrome-analysis-output PATH` | bounded directory | unset | Enables bounded one-shot Chrome report import in the standalone lifecycle; requires `--chrome-analysis-queue`. |
| `--otel-endpoint HTTPS_URL` | HTTPS collector URL | unset | Enables OTLP/HTTP JSON export only with `--otel-trace-queue`; authorization is read only from `LAGHU_OTEL_AUTHORIZATION`. |
| `--otel-trace-queue PATH` | bounded path | unset | Dedicated bounded async trace queue, consumed by `laghu-otel-export`. |
| `--otel-sampling-rate 0..100` | percentage | `0` | Sampling is disabled by default; queue saturation drops spans without affecting responses. |
| `--otel-ca-file PATH` | PEM trust bundle | system trust | Optional collector trust bundle. |
| `--layout-reservation-config PATH` | bounded rule file | unset | Loads exact-ID `box ID WIDTH HEIGHT` and `font ID ADJUST_MILLI` CLS reservations. Rules apply only to learned CLS regressions and CSP-permitted style attributes. |
| `--chrome-analysis-timeout MS` | `100..10000` | `1500` | Bounds one analysis job; requires `--chrome-analysis-queue`. |
| `--asset-offload-config PATH` | valid asset policy file | unset | Enables verified immutable CDN rewriting. |
| `--asset-upload-queue PATH` | policy-matching path | unset | Selects the asynchronous asset spool. |
| `--load-from-file off\|mapped` | source-loader mode | `off` | Enables asynchronous explicitly mapped file acquisition. |
| `--file-source-map SOURCE_PREFIX=ROOT` | normalized HTTPS prefix and absolute local root, repeatable to 8 | none | Maps an eligible origin prefix to a local file tree. |
| `--javascript-target QUERY` | bounded Browserslist query | `defaults and supports es6-module and not dead` | Controls syntax lowering. |
| `--javascript-inline-limit N` | `0..65536` | `2048` | Caps JavaScript inlining. |
| `--javascript-outline-threshold N` | `1024..1048576` | `8192` | Selects inline scripts for outlining. |
| `--javascript-observation-config PATH` | valid observation file | unset | Adds exact third-party script candidates. |
| `--javascript-defer-config PATH` | valid approval file | unset | Approves exact same-origin deferrals and exact third-party interaction delays. |
| `--workers N` | `1..256` | `4` | Sets request worker threads. |
| `--connection-queue N` | `1..65536` | `64` | Bounds accepted connections waiting for workers. |
| `--connect-timeout SECONDS` | `1..300` | `5` | Bounds origin connection establishment. |
| `--io-timeout SECONDS` | `1..300` | `30` | Bounds client/origin I/O. |
| `--origin-pool-size N` | `0..1024` | `16` | Bounds idle HTTP/1.1 origin connections; `0` disables reuse. |
| `--origin-idle-timeout SECONDS` | `1..3600` | `30` | Closes an idle retained origin connection before reuse. |
| `--drain-timeout SECONDS` | `1..300` | `30` | Bounds graceful shutdown. |
| `--origin-ca-file PATH` | CA bundle path | platform trust | Overrides trust for HTTPS origins; invalid with HTTP. |
| `--tls-certificate PATH` | absolute PEM path | disabled | Enables standalone HTTPS only with `--tls-private-key`. |
| `--tls-private-key PATH` | absolute PEM path | disabled | Private key paired with `--tls-certificate`; invalid alone. |
| `--pid-file PATH` | absolute path | disabled | Creates the protected process-id file used by `laghu reload`. |
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

## Reload

Set `runtime.pid_file` and run `laghu reload --config /etc/laghu/laghu.yaml`. The command first performs the same strict YAML,
policy, service, and path validation as startup, then atomically publishes that absolute candidate path beside the PID file before
signaling the running process. The PID directory must be owned by the running user and not group- or world-writable. The daemon
claims the owner-only, no-follow request file, removes it before loading, and constructs a separate immutable replacement from that
exact path. It atomically publishes only a validated compatible snapshot. Startup removes stale request state, and every consumed
request is removed before validation, so an invalid request cannot replay later. Invalid or incompatible input leaves the last valid
configuration live and emits a `reload` `retained` lifecycle record; a successful swap emits `reload` `applied`.

Reload changes request-policy, route, and static-site settings without stopping workers. Listener, worker, connection-pool,
cache/queue/RUM/source, PID-file, and TLS/SNI topology are lifecycle-owned, so changes to them require a normal restart and are
retained rather than partially applied. The PID path itself must stay stable across a reload.

## Downstream TLS and SNI virtual hosts

Provide both `--tls-certificate` and `--tls-private-key` to terminate TLS 1.2+ for the standalone HTTP/1.1 listener.
Laghu validates the PEM chain and matching private key before it opens the listener. Omit both options for the existing plaintext listener;
Laghu does not mix plaintext and TLS on one port. A `sites` entry may instead provide its own paired `tls_certificate` and
`tls_private_key`; Laghu selects that certificate by exact case-insensitive SNI hostname before HTTP routing. Use a global pair as
the default certificate, or let the first TLS site provide it. Every TLS site needs both files and hostnames must be unique.
Certificate changes require restart. Client certificates and HTTP/2 or HTTP/3 listener negotiation are not configured by these options.

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

Filter names are `image_lossless`, `image_metadata`, `image_dimensions`, `image_modern`, `image_responsive`, `image_lazyload`, `html_minify`, `css_minify`, `javascript_minify`, `resource_hints`, `cache_extension`, `resource_combine`, `resource_inline`, `critical_css`, `javascript_defer`, `immutable_cache`, and `cache_media`.
Each filter may be controlled only once; duplicates and conflicts fail startup, as does enabling a filter with `passthrough`.
Resource patterns begin with `/`, `http://`, or `https://`; support `*` and `?`; and match normalized URLs without fragments. Query overrides cannot bypass forbidden filters or denied resources, remain origin-visible, and participate in cache identity. Encode `+` as `%2B` for form-style clients.

The file cache is the only cache backend currently implemented. Its portable shared mapping contains bounded keys, sizes, state, and approximate-LRU access epochs only; response bodies and generated assets remain on disk. Unsupported schemes such as `memcached:` fail startup so a future provider can implement the same contract without changing callers.
Remote RUM synchronization requires verified `rediss://`; `redis://` is loopback-only and Memcached is unsupported.
Mapped loading requires asset offload configuration and remains capture-first and worker-only. Standalone rejects `native` and `both` because its origin is remote. Roots, every path component, and the final regular file are checked; symlinks, traversal, reparse points, device paths, and changed files fail open.

## Status client

`laghu status URL --token-file PATH` queries the existing authenticated
`/.laghu/ready` and `/.laghu/stats` routes on standalone, NGINX, or Apache.
`URL` must be a root `http://` or `https://` origin; paths, credentials,
queries, and fragments are rejected.

Create the client token file with the same token configured for the endpoint.
It must be an absolute, regular file owned by the invoking user, inaccessible
to group and other users, and contain a 16-256 byte printable non-whitespace
token (a final newline is allowed):

```sh
install -m 600 /dev/stdin /etc/laghu/status.token
laghu status https://edge.example.com --token-file /etc/laghu/status.token
```

The command sends the token only as `X-Laghu-Purge-Token`; it never prints the
token, request header, or reconstructed command. The default five-second
timeout applies to the request operations and can be set to `1..30` seconds.
For a private HTTPS CA, `--ca-file /etc/laghu/edge-ca.pem` adds trust without
disabling certificate or hostname verification. `--json` emits a
`laghu-status-v1` object; ordinary output is one readiness line and one cache
hit/miss/usage/capacity line.

| Exit | Meaning |
| --- | --- |
| 0 | Both routes returned valid `200` JSON. |
| 2 | Invalid command, URL, or token file. |
| 3 | Either route returned `401` or `403`. |
| 4 | DNS, connection, TLS, or I/O failure. |
| 5 | Invalid HTTP framing, content, or JSON schema. |
| 6 | Either route returned `503`; stats is still queried after readiness `503`. |
| 7 | Another HTTP status. |

## Doctor client

`laghu doctor URL --token-file PATH` uses the same authenticated, bounded
transport as `laghu status`, then reports runtime, cache, worker, budget, and
policy readiness alongside cache hit/miss/capacity statistics. It works
unchanged against standalone, NGINX, and Apache routes. `--json` emits the
versioned `laghu-doctor-v1` object. Its token, URL, timeout, CA, and exit-code
rules are identical to `laghu status`.

```sh
laghu doctor https://edge.example.com --token-file /etc/laghu/status.token
```

## Purge client

`laghu purge URL --token-file PATH` sends authenticated `PURGE` to the full `http://` or `https://` resource URL on standalone, NGINX, or Apache.

The URL may include its cache source path and query; a missing path becomes `/`. Credentials, fragments, malformed targets, the reserved `laghu=purge` query control, and targets longer than 1023 bytes are rejected. The client preserves the accepted encoded path and query, follows no redirects, and sends the origin authority as `Host`.

`--timeout 1..30`, HTTPS-only `--ca-file`, and the protected token-file rules are identical to `status`. The command never prints the token, request header, input URL, or server response body. Normal output is `purge: accepted matched_artifacts=N`; `--json` emits `laghu-purge-v1` with the same count.

| Exit | Meaning |
| --- | --- |
| 0 | Valid `202` accepted response. |
| 2 | Invalid command, URL, token file, or server-rejected target (`400`). |
| 3 | `401` or `403`. |
| 4 | DNS, connection, TLS, or I/O failure. |
| 5 | Invalid HTTP framing or accepted JSON response. |
| 6 | `503` service unavailable. |
| 7 | `404`, `405`, or another unexpected HTTP status. |
| 8 | `429` purge table saturated; retry after maintenance. |

## Bench client

`laghu bench URL [--requests N]` sends repeated GET requests and emits simple latency
and success metrics. `URL` follows the same origin+path parser as purge and explain:
root `http://` or `https://` origin, optional root-relative path, and query only.

Optional flags:

- `--requests 1..100000` — number of requests to send (default `100`).
- `--timeout 1..30` — per-request timeout in seconds (default `5`).
- `--ca-file PATH` — HTTPS trust anchor; requires HTTPS URL.
- `--json` — emit `laghu-bench-v1`.

```sh
laghu bench https://edge.example.com/index.html --requests 200
```

Plain output prints aggregate counters, throughput, and latency. `--json` prints
`laghu-bench-v1` with `target`, `requests`, `success`, `failures`, `status_4xx`,
`status_5xx`, `bytes`, and `min_ms`/`max_ms`/`avg_ms`.

| Exit | Meaning |
| --- | --- |
| 0 | All requests returned `200`. |
| 2 | Invalid command, URL, or option. |
| 4 | I/O/TLS/connect timeout for any request. |
| 5 | Malformed HTTP framing or body in at least one response. |
| 7 | One or more requests returned a non-`200` status. |

## JavaScript deferral, interaction approvals, and task yields

The approval file accepts exact, root-relative, query-free same-origin deferrals and exact HTTPS third-party interaction delays, each optionally scoped to one exact template:

```text
defer /assets/analytics.js
defer /assets/checkout.js template=/checkout/
interaction https://cdn.example.test/analytics.js template=/checkout/
```

Interaction URLs must be default-port HTTPS URLs without credentials, query, or fragment, and must exactly match a third-party script already present in the response. Laghu delays only a classic external script with no body and no attributes except `src` and an optional safe nonce. The defer filter and CSP must permit both the original external script and Laghu's nonce-bearing loader; otherwise markup passes through unchanged. First `pointerdown`, `keydown`, or `touchstart` loads the approved script in document order. Instrumentation and the defer filter must both be enabled. Deferral suggestions require 100 fresh bucket observations, at least 90 percent script coverage, current SWC safety metadata, and safe ordering. Reload the service after editing the file. Approval never bypasses current evidence; rollback notices require 50 post-enable observations, and removing the line plus reloading performs the rollback.

To split an application-owned long task, mark its inline script `<script data-laghu-yield="cooperative" nonce="…">` and explicitly `await window.Laghu.yield()` between independent chunks. With the defer filter enabled, Laghu inserts a nonce-preserving helper before that script. The helper prefers `scheduler.postTask`, then `requestIdleCallback`, then `setTimeout`, and preserves an existing `Laghu.yield`. Missing or invalid marker, missing/invalid nonce, or CSP denial leaves markup unchanged.
