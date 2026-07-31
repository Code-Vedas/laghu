---
title: Configuration
nav_order: 3
permalink: /configuration/
---

# Configuration

Both modules expose the same configuration semantics. NGINX uses the lowercase `laghu` directive with semicolons; Apache uses `Laghu` without semicolons. Unsupported settings fail server configuration validation.

| NGINX | Apache HTTP Server |
| --- | --- |
| `laghu on;` | `Laghu On` |
| `laghu preset balanced;` | `Laghu Preset balanced` |
| `laghu rewrite_level core;` | `Laghu RewriteLevel core` |
| `laghu allow_api off;` | `Laghu AllowApi Off` |
| `laghu image_quality 82;` | `Laghu ImageQuality 82` |
| `laghu image_beacon off;` | `Laghu ImageBeacon Off` |
| `laghu critical_css_beacon off;` | `Laghu CriticalCssBeacon Off` |
| `laghu instrumentation_beacon off;` | `Laghu InstrumentationBeacon Off` |
| `laghu instrumentation_sample_rate 10;` | `Laghu InstrumentationSampleRate 10` |
| `laghu javascript_observation_config /etc/laghu/javascript-observation.conf;` | `Laghu JavaScriptObservationConfig /etc/laghu/javascript-observation.conf` |
| `laghu image_inline_limit 2048;` | `Laghu ImageInlineLimit 2048` |
| `laghu image_metadata_limit 10000;` | `Laghu ImageMetadataLimit 10000` |
| `laghu image_metadata_ttl 7d;` | `Laghu ImageMetadataTtl 7d` |
| `laghu css_inline_limit 2048;` | `Laghu CssInlineLimit 2048` |
| `laghu css_outline_threshold 8192;` | `Laghu CssOutlineThreshold 8192` |
| `laghu javascript_inline_limit 2048;` | `Laghu JavaScriptInlineLimit 2048` |
| `laghu javascript_outline_threshold 8192;` | `Laghu JavaScriptOutlineThreshold 8192` |
| `laghu rum_store local:;` | `Laghu RumStore local:` |
| `laghu rum_store_local_snapshot /var/lib/laghu/rum/rum.snapshot;` | `Laghu RumStoreLocalSnapshot /var/lib/laghu/rum/rum.snapshot` |
| `laghu rum_store_client_library /usr/lib/libhiredis.so;` | `Laghu RumStoreClientLibrary /usr/lib/libhiredis.so` |
| `laghu rum_store_timeout 100;` | `Laghu RumStoreTimeout 100` |
| `laghu rum_store_ttl 604800;` | `Laghu RumStoreTtl 604800` |
| `laghu rum_store_retry_limit 3;` | `Laghu RumStoreRetryLimit 3` |
| `laghu rum_store_sync_interval 5;` | `Laghu RumStoreSyncInterval 5` |
| `laghu rum_store_memory_limit 8m;` | `Laghu RumStoreMemoryLimit 8m` |
| `laghu rum_store_pending_limit 1m;` | `Laghu RumStorePendingLimit 1m` |
| `laghu rum_store_required off;` | `Laghu RumStoreRequired Off` |

## `laghu on|off`

Enables or disables Laghu for the current scope.

```nginx
laghu on;
```

- Context: `http`, `server`, `location`
- Default: `off`
- Inheritance: child scopes inherit their parent unless overridden

When enabled, Laghu evaluates the response and may enqueue eligible images or serve an already-published image variant.

## `laghu preset <name>`

Selects an optimization policy preset:

```nginx
laghu preset balanced;
```

Accepted names are `safe`, `balanced`, `aggressive`, `ecommerce`, `blog`, and `static`. Each resolves to a tested family mask, risk level, and set of transformation permissions consumed by available filters.

- Context: `http`, `server`, `location`
- Default: `balanced`

| Preset | Selected policy |
| --- | --- |
| `safe` | Lossless image and metadata work without lossy, structural, inline, combine, or script-order changes |
| `balanced` | Recommended image, markup, minification, and resource-hint permissions without inlining, combining, critical CSS, or script reordering |
| `aggressive` | Every defined family with lossy, structural, inline, combine, and script-reordering permission |
| `ecommerce` | Product-image, markup, CSS, and resource-hint permissions without HTML or JavaScript rewriting |
| `blog` | Balanced permissions plus resource inlining and script reordering |
| `static` | Aggressive transformation permissions with a distinct immutable-cache policy identity |

Universal authorization, privacy, fail-open, deterministic-output, and never-larger safeguards cannot be disabled by a preset.

## `laghu rewrite_level <name>`

Selects a compatibility-oriented filter-family policy:

```nginx
laghu rewrite_level core;
```

| Level | Selected policy |
| --- | --- |
| `passthrough` | No filter families; the enabled module always preserves the original |
| `core` | The balanced, recommended-default family set |
| `bandwidth` | Image and text byte-reduction permissions without structural, inline, combine, critical-CSS, or script-order changes |
| `all` | Every defined family with expansive permissions |
| `experimental` | `all` plus explicit permission for experimental filters |

- Context: `http`, `server`, `location`
- Inheritance: child scopes inherit their parent unless overridden
- Default: no rewrite level; the default selector remains `preset balanced`

`laghu preset` and `laghu rewrite_level` are alternative policy selectors and cannot appear together in one scope. A child scope may replace an inherited preset with a rewrite level, or an inherited rewrite level with a preset.

## Image Runtime Directives

```nginx
laghu image_quality 82;
laghu image_beacon off;
laghu critical_css_beacon off;
laghu instrumentation_beacon off;
laghu instrumentation_sample_rate 10;
laghu javascript_observation_config /etc/laghu/javascript-observation.conf;
laghu image_inline_limit 2048;
laghu image_metadata_limit 10000;
laghu image_metadata_ttl 7d;
laghu css_inline_limit 2048;
laghu css_outline_threshold 8192;
laghu worker_queue /run/laghu/jobs.queue;
laghu font_fetch_queue /run/laghu/fonts.queue;
laghu font_provider_config /etc/laghu/font-providers.conf;
laghu javascript_queue /run/laghu/javascript.queue;
laghu javascript_target "defaults and supports es6-module and not dead";
laghu javascript_inline_limit 2048;
laghu javascript_outline_threshold 8192;
laghu image_cache /var/cache/laghu/images;
```

All image runtime directives inherit through `http`, `server`, and `location`. Quality must be `1..100`. Without an override, ecommerce uses 85; balanced/core/blog/bandwidth use 82; and aggressive/static/all/experimental use 75. Safe permits no lossy output, so an inherited quality does not relax it.

The queue and cache paths default to the values above. `laghu-libvips` must have write access; the selected server needs queue access and read access to published cache entries.

External font CSS uses the separate bounded `font_fetch_queue` and `laghu-resource-fetch` service. `font_provider_config` names the administrator-owned provider file loaded by both the adapter and worker; invalid files fail configuration loading. Apache exposes the same settings as `Laghu FontFetchQueue` and `Laghu FontProviderConfig`. The standalone proxy uses `--font-fetch-queue` and `--font-provider-config`.

JavaScript rewriting uses the separate bounded `javascript_queue` and native `laghu-js-optimize` Rust service. The inherited `javascript_target` is a bounded Browserslist query and defaults to `defaults and supports es6-module and not dead`; file-loading, environment-expanding, `extends`, empty, and malformed queries are rejected. Apache exposes `Laghu JavaScriptQueue` and `Laghu JavaScriptTarget`, while standalone uses `--javascript-queue` and `--javascript-target`. Laghu observes same-origin external scripts only through ordinary responses and never fetches JavaScript. Inline and external classic/module scripts remain byte-identical until a strictly smaller SWC result is ready. Import maps, data scripts, speculation rules, integrity-protected external scripts, source-map directives, and CSP-hash-sensitive inline scripts without an accepted nonce remain unchanged.

JavaScript inlining defaults to 2 KiB and accepts `0..65536`; zero disables it. Outlining considers executable inline scripts from 8 KiB and accepts `1024..1048576`. The equivalent Apache settings are `Laghu JavaScriptInlineLimit` and `Laghu JavaScriptOutlineThreshold`; standalone uses `--javascript-inline-limit` and `--javascript-outline-threshold`. Inlining and outlining require ready SWC output with a checksummed URL-independence certificate, compatible attributes and CSP, and a strictly smaller combined HTML-plus-JavaScript transfer.

Classic scripts may opt into combination by placing the same bounded `data-laghu-combine="group"` value on 2 through 16 whitespace-adjacent blocking external scripts. Every member must have compatible attributes and SWC's concatenation-safety certificate. Modules, integrity-protected scripts, asynchronous or deferred scripts, declarations that expose global boundaries, URL-sensitive code, source directives, and partially ready groups remain unchanged. Accepted bundles preserve DOM order with explicit statement separators and use immutable `/.laghu/js/<sha256>` routes.

The line-oriented provider format starts a definition with `provider ID`, ends it with `end`, and accepts `stylesheet HOST PATH_PREFIX`, `redirect HOST PATH_PREFIX`, `asset HOST PATH_PREFIX`, `max_css_bytes BYTES`, and `ttl_seconds SECONDS`. Hosts and prefixes are exact and do not accept wildcards, credentials, IP literals, regexes, or HTTP. The installed file enables Google Fonts and Fontsource CDN; adding another provider requires only a normal configuration reload.

Eligible links remain unchanged on a cold miss or any queue, worker, network, TLS, parser, CSP, or cache failure. Ready CSS is inlined only when its media and supported attributes can be preserved and the resulting HTML is no larger. The fixed WOFF2-oriented fetch profile does not forward browser headers, cookies, credentials, client addresses, or proxy headers, and referenced font binaries remain external.

Beaconing is inherited and disabled by default. `critical_css_beacon` is independent from image beaconing and is exposed as `Laghu CriticalCssBeacon` or standalone `--critical-css-beacon`. Inline payloads default to a 2 KiB maximum. Image metadata and learned critical-CSS observations use the configured seven-day metadata TTL.

RUM instrumentation is independently opt-in through `instrumentation_beacon`, `Laghu InstrumentationBeacon`, or standalone `--instrumentation-beacon`. Its client-side sampling rate defaults to 10 percent and accepts `0..100`; HTML remains deterministic because each browser makes the sampling decision. Eligible complete documents receive one deferred same-origin script before `</body>`. Hash-only, nonce-only, `strict-dynamic`, malformed, and oversized documents remain unchanged.

The administrator-owned JavaScript observation file uses one strict directive per line: `host exact.example /path/prefix/`. Prefixes must end in `/`. Wildcards, IP literals, credentials, URLs, overlapping entries, unsafe paths, and unknown fields fail configuration loading. Same-origin scripts are always cataloged; configured exact HTTPS host and path pairs add bounded third-party candidates. A normal server reload applies file changes.

The fixed script is served at `/.laghu/beacon/instrumentation.js` and submits at most one bounded same-origin JSON report to `/.laghu/beacon/instrumentation` during `pagehide`. Reports contain an opaque template key, a mobile or desktop bucket, bounded Core Web Vitals and lifecycle values, error counters without messages or stacks, and opaque hashes for cataloged scripts. Laghu stores checksummed aggregate histograms and counters only; it never stores raw reports, complete URLs, queries, cookies, addresses, client identifiers, page text, selectors, error messages, or stacks. Unsupported browser timing APIs simply omit useful observations. Aggregate expiry uses `image_metadata_ttl`.

RUM records are memory-native. Every native worker serves planners and beacon merges from a bounded in-process store while a background thread restores, merges, and publishes records. No request or optimization path performs file or Redis I/O. The default `local:` backend uses `<image-cache>/rum.snapshot`; `memory:` disables persistence; and Redis shares learning among replicas. NGINX accepts the following only in the `http` context, Apache only in the main server context, and standalone exposes the equivalent `--rum-store-*` options:

```text
laghu rum_store local:;
laghu rum_store_local_snapshot /var/lib/laghu/rum/rum.snapshot;
laghu rum_store_timeout 100;
laghu rum_store_ttl 604800;
laghu rum_store_retry_limit 3;
laghu rum_store_sync_interval 5;
laghu rum_store_memory_limit 8m;
laghu rum_store_pending_limit 1m;
laghu rum_store_required off;
```

The Apache setting names are `RumStore`, `RumStoreLocalSnapshot`, `RumStoreClientLibrary`, `RumStoreTimeout`, `RumStoreTtl`, `RumStoreRetryLimit`, `RumStoreSyncInterval`, `RumStoreMemoryLimit`, `RumStorePendingLimit`, and `RumStoreRequired` beneath the existing `Laghu` directive. These settings are process-wide: NGINX accepts them only in `http`, and Apache only in the main server configuration. Store failures are fail-open unless required mode is selected. Credentials must never be placed in the local snapshot path or diagnostic output.

Debian, RPM, Homebrew, and container configurations explicitly select `local:` with a persistent snapshot under the platform data directory. Windows matched-server packages use `%ProgramData%\Laghu\rum\rum.snapshot`. The installers create the parent directory with access for the server worker. Redis remains an administrator-selected replacement because its library path, credentials, TLS trust, endpoint, and durability policy are deployment-specific.

Redis is optional and loaded dynamically from administrator-installed hiredis 1.x; Laghu packages do not bundle or download it. The loader rejects pre-1.0 client ABIs before creating a connection. Use verified TLS for every non-loopback server:

```text
laghu rum_store "rediss://${RUM_USERNAME}:${RUM_PASSWORD}@persistent-redis.example.net:6380/0?prefix=laghu:&ca_file=/etc/ssl/certs/rum-ca.pem";
laghu rum_store_client_library /usr/lib/libhiredis.so;
laghu rum_store_required off;
```

`redis://` is accepted only for `localhost`, `127.0.0.1`, or `::1` development endpoints. `rediss://` enables peer verification and SNI through hiredis SSL; `ca_file` selects an administrator-owned CA bundle, while omission uses the platform trust configuration provided by hiredis. URI fields may use bounded `${NAME}` environment expansion and percent-encoded credentials. Laghu never includes credentials in policy keys, snapshots, browser output, or diagnostic messages.

The Redis endpoint must be independent of disposable Laghu servers and containers if learning must survive their replacement. Use a managed Redis service, a dedicated persistent host, or a separately operated Redis deployment backed by durable storage. Redis running inside an ephemeral Laghu container does not provide persistence; Redis durability also depends on its own AOF, snapshot, and storage configuration.

Redis and Valkey synchronization uses one versioned, bounded Lua merge contract without a distributed lock. A worker rotates at most 64 pending records under its memory mutex, releases that mutex before network I/O, and submits their portable encodings with an idempotent batch ID. The script validates the complete batch, applies byte-exact maxima, unions, and saturating additions atomically, updates record and index TTLs, and writes the batch marker last. It uses byte operations rather than Lua numbers for 64-bit counters. A lost response is retried with the same batch ID and cannot double-count observations. The returned aggregates refresh the worker's in-memory view and optional last-known-good snapshot. When Redis or Valkey is unavailable and required mode is off, startup warns and falls back to `local:`. Required mode refuses worker startup.

Memcached is intentionally unsupported in this version because its eviction model cannot provide the durable idempotent batch contract. `memcached:` URIs fail configuration validation rather than silently weakening the merge semantics.

Stylesheet inlining defaults to 2 KiB and accepts `0..65536`; zero disables it. Outlining considers complete inline style blocks from 8 KiB by default and accepts `1024..1048576`. Inlining requires a ready same-origin stylesheet, inline-style CSP permission, and the strict integrity/nonce/media/import/font eligibility checks. Outlining requires structural-rewrite permission. Both preserve the first HTML response and apply only after their catalog dependency is ready and the combined HTML/CSS transfer is smaller.

No separate directive controls stylesheet combination. Policies that select CSS rewriting and permit structural rewriting may combine up to 32 compatible, whitespace-adjacent links after inlining has run. Media, CSP, source-map, import, font, catalog-readiness, and strict total-transfer checks remain mandatory.

Head normalization and CSS placement have no separate directives. They are derived from resolved policy: HTML rewriting plus structural permission enables missing/adjacent-head normalization, and CSS rewriting additionally enables eligible stylesheet and style-block movement. CSS crosses executable scripts only when the selected policy explicitly permits script reordering. Every path keeps the first eligible response unchanged and accepts byte-neutral structural placement.

Policies that select HTML minification also enable four conservative lexical filters, including the non-structural `bandwidth` rewrite level. Laghu collapses ASCII whitespace only in ordinary text, removes only unprotected complete comments, unquotes only values valid in HTML's unquoted syntax, and elides only exact default `text/javascript` and `text/css` MIME attributes. It preserves `pre`, `textarea`, script/style, template/noscript, SVG/MathML, legacy raw-text, and content-editable regions. Conditional comments and comments containing `!`, `@license`, `@preserve`, `laghu:keep`, `sourceMappingURL`, or `sourceURL` are retained. Malformed or ambiguous markup remains byte-identical.

Lexical output must be strictly smaller and is reparsed before publication. The first eligible response remains the origin response; a later warm response uses a source-, policy-, planner-mask-, and dependency-derived strong ETag. Laghu does not expose per-filter directives.

Outlined assets are exposed only through the validated `/.laghu/css/<sha256>` route with `text/css`, a strong ETag, and one-year immutable caching. Laghu never fetches a stylesheet from an origin.

Cold HTML is preserved while Laghu discovers image dependencies. Warm HTML is rewritten only after every dependency is ready or terminally excluded. When enabled, `image_beacon` serves a fixed same-origin script and accepts bounded same-origin JSON observations at `/.laghu/beacon/images`; it does not store cookies, IP addresses, client identifiers, or page content. The reserved `/.laghu/image/<hash>` route serves only validated content-addressed variants.

Critical-CSS beaconing serves a fixed script at `/.laghu/beacon/critical-css.js` and accepts same-origin observations at `/.laghu/beacon/critical-css`. Reports contain only an opaque template key, a mobile/desktop bucket, and bounded rule indexes. Three observations are required before Laghu may inline matching rules. Learning is monotonic and invalidated by template, stylesheet, policy, or parser changes. Eligible pages have one final ordinary same-origin stylesheet and no inline author style block. Laghu places learned critical rules at the original link position and the complete derived stylesheet immediately before `</body>`, preserving the final cascade without depending on JavaScript. The existing `css_inline_limit` bounds the temporary duplicated bytes.

## `laghu allow_api on|off`

Laghu bypasses `/api`, `/api/...`, `/graphql`, and `/graphql/...` by default. The comparison is case-sensitive and observes path-segment boundaries, so `/apiary` and `/graphql-ui` are not classified as API paths.

Use a narrow override only when an API-namespaced location serves optimizable HTML, CSS, JavaScript, images, or fonts:

```nginx
location /api/public-assets/ {
  laghu allow_api on;
}
```

- Context: `http`, `server`, `location`
- Default: `off`
- Inheritance: child scopes inherit their parent unless overridden

The override disables only path classification. Authorization, `private`, `no-store`, status, and content-type safeguards still apply.

## Decision Header

An enabled location returns one of these `X-Laghu` values:

| Value | Meaning |
| --- | --- |
| `pass` | Response is eligible for optimization |
| `bypass-passthrough` | Passthrough level intentionally selected no filter families |
| `bypass-status` | Response status is not eligible |
| `bypass-authorized` | Request carried authorization credentials |
| `bypass-private` | Response is marked `private` or `no-store` |
| `bypass-api` | Request path is API-classified and has no explicit override |
| `bypass-content-type` | Content type is outside the optimization surface |
| `bypass-encoded` | Origin bytes already carry a content encoding and cannot be safely substituted |
| `bypass-image-backend` | No selected transform is supported by the published worker capability mask |
| `image-hit` | A strong-validator lookup selected an atomic cached image variant |
| `bypass-error` | Policy metadata could not be inspected safely |

Only `image-hit` claims variant delivery. `pass` on an image cold miss means the original was streamed and an optimization job may have been published.

When multiple conditions apply, Laghu evaluates disabled configuration, invalid policy data, intentional passthrough, status, authorization, private caching, API path, and content type in that order.

## Scoped Configuration

```nginx
server {
  laghu on;

  location /unoptimized/ {
    laghu rewrite_level passthrough;
  }

  location /api/public-assets/ {
    laghu allow_api on;
  }
}
```
