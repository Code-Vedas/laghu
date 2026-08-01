---
title: Configuration
parent: mod-laghu
nav_order: 2
permalink: /mod-laghu/configure/
---

# Configure mod-laghu

Apache exposes one `Laghu` directive.
Optimization settings inherit through main server, virtual host, directory, and location configuration; RUM-store settings are accepted only in main server configuration.

| Directive | Context | Accepted value | Default | Effect |
| --- | --- | --- | --- | --- |
| `Laghu On\|Off` | inherited | boolean | `Off` | Enables or bypasses transformation. |
| `Laghu Preset NAME` | inherited | `safe`, `balanced`, `aggressive`, `ecommerce`, `blog`, `static` | `balanced` | Selects a policy preset. |
| `Laghu RewriteLevel NAME` | inherited | `passthrough`, `bandwidth`, `core`, `testing` | unset | Selects a rewrite level instead of a preset. |
| `Laghu AllowApi On\|Off` | inherited | boolean | `Off` | Allows otherwise excluded API/GraphQL paths. |
| `Laghu ImageQuality N` | inherited | `1..100` | codec default | Overrides lossy image quality. |
| `Laghu ImageBeacon On\|Off` | inherited | boolean | `Off` | Enables critical-image observations. |
| `Laghu CriticalCssBeacon On\|Off` | inherited | boolean | `Off` | Enables critical-CSS learning. |
| `Laghu InstrumentationBeacon On\|Off` | inherited | boolean | `Off` | Injects bounded RUM instrumentation when CSP permits. |
| `Laghu InstrumentationSampleRate N` | inherited | `0..100` | `10` | Sets browser-side percentage sampling. |
| `Laghu JavaScriptDeferSuggestions On\|Off` | inherited | boolean | `On` | Enables bounded RUM deferral recommendations; it never applies them. |
| `Laghu IncludeJsSourceMaps On\|Off` | inherited | boolean | `Off` | Emits immutable external SWC source maps without source content. |
| `Laghu ImageInlineLimit BYTES` | inherited | `0..16384` | `2048` | Caps image inlining. |
| `Laghu ImageMetadataLimit N` | inherited | `1..100000` | `10000` | Bounds learned image records. |
| `Laghu ImageMetadataTtl DURATION` | inherited | `1h..30d` | `7d` | Expires learned metadata. |
| `Laghu CssInlineLimit BYTES` | inherited | `0..65536` | `2048` | Caps inline and critical CSS. |
| `Laghu CssOutlineThreshold BYTES` | inherited | `1024..1048576` | `8192` | Selects large inline CSS for outlining. |
| `Laghu JavaScriptInlineLimit BYTES` | inherited | `0..65536` | `2048` | Caps external JavaScript inlining. |
| `Laghu JavaScriptOutlineThreshold BYTES` | inherited | `1024..1048576` | `8192` | Selects inline JavaScript for outlining. |
| `Laghu WorkerQueue PATH` | inherited | bounded path | `/run/laghu/jobs.queue` | Selects the image queue. |
| `Laghu AssetOffloadConfig PATH` | inherited | valid asset policy file | unset | Enables verified immutable CDN rewriting. |
| `Laghu AssetUploadQueue PATH` | inherited | policy-matching path | unset | Selects the asynchronous asset spool. |
| `Laghu FontFetchQueue PATH` | inherited | bounded path | `/run/laghu/fonts.queue` | Selects the font-fetch queue. |
| `Laghu FontProviderConfig PATH` | inherited | valid provider file | unset | Enables configured external-font providers. |
| `Laghu JavaScriptQueue PATH` | inherited | bounded path | `/run/laghu/javascript.queue` | Selects the SWC queue. |
| `Laghu JavaScriptTarget QUERY` | inherited | bounded Browserslist query | `defaults and supports es6-module and not dead` | Controls syntax lowering. |
| `Laghu JavaScriptObservationConfig PATH` | inherited | valid observation file | unset | Adds exact third-party script candidates. |
| `Laghu JavaScriptDeferConfig PATH` | inherited | valid approval file | unset | Approves exact same-origin scripts for evidence-gated deferral. |
| `Laghu ImageCache PATH` | inherited | bounded path | `/var/cache/laghu/images` | Selects catalogs and immutable assets. |
| `Laghu RumStore URI` | main server | `memory:`, `local:`, supported Redis URI | `local:` | Selects RUM persistence/synchronization. |
| `Laghu RumStoreLocalSnapshot PATH` | main server | bounded path | `<ImageCache>/rum.snapshot` | Selects last-known-good snapshot storage. |
| `Laghu RumStoreClientLibrary PATH` | main server | hiredis library path | unset | Enables runtime-loaded Redis/Valkey support. |
| `Laghu RumStoreTimeout MS` | main server | `10..10000` | `100` | Bounds backend operations. |
| `Laghu RumStoreTtl SECONDS` | main server | `3600..2592000` | `604800` | Sets aggregate expiry. |
| `Laghu RumStoreRetryLimit N` | main server | `0..10` | `3` | Bounds synchronization retries. |
| `Laghu RumStoreSyncInterval SECONDS` | main server | `1..300` | `5` | Sets synchronization cadence. |
| `Laghu RumStoreMemoryLimit BYTES` | main server | `16KiB..1GiB`, size suffix accepted | `64MiB` | Bounds resident RUM records. |
| `Laghu RumStorePendingLimit BYTES` | main server | `16KiB..1GiB`, size suffix accepted | `16MiB` | Bounds unsynchronized deltas. |
| `Laghu RumStoreRequired On\|Off` | main server | boolean | `Off` | Makes RUM initialization failure fatal. |

`Preset` and `RewriteLevel` cannot appear together in the same scope.
Provider and observation files are validated during configuration loading.
Remote synchronization requires verified `rediss://`; `redis://` is restricted to loopback development, and Memcached is unsupported.

```apache
Laghu RumStore local:
Laghu RumStoreLocalSnapshot /var/lib/laghu/rum/rum.snapshot

<VirtualHost *:80>
  Laghu On
  Laghu Preset balanced
</VirtualHost>
```

## JavaScript deferral approvals

The approval file accepts exact, root-relative, query-free script paths, optionally scoped to one exact template:

```text
defer /assets/analytics.js
defer /assets/checkout.js template=/checkout/
```

Instrumentation and the defer filter must both be enabled. Suggestions require 100 fresh bucket observations, at least 90 percent script coverage, current SWC safety metadata, and safe ordering. Run the Apache configuration test and reload after editing the file. Approval never bypasses current evidence; rollback notices require 50 post-enable observations, and removing the line plus reloading performs the rollback.
