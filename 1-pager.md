# Laghu

## Positioning

**Laghu is a free, self-hostable web-content optimization engine with three equal deployment surfaces: native `ngx-laghu` for NGINX, native `mod-laghu` for Apache, and standalone `laghu` reverse proxy for any HTTP origin.**

- Shared server-neutral engine; zero application changes.
- Modern replacement path for archived `ngx_pagespeed` / `mod_pagespeed`: current formats, CWV, protocols, observability, and operations.
- `laghu` (लघु): Sanskrit for light, quick, small, nimble.
- MIT, self-hostable, no account, license key, paid tier, feature gate, mandatory telemetry, or hosted dependency.

## Naming

- Runtime/public namespace only: `laghu`, `X-Laghu`, `/laghu/...`.
- No PageSpeed branding in runtime directives, headers, CLI, config, endpoints, packages, or images.
- Legacy names only in one historical docs section, migration input parser, and internal audit/bench material.

## Users

- NGINX/Apache operators replacing PageSpeed.
- WordPress/WooCommerce/Magento/shared-hosting/SaaS operators needing server-level optimization.
- Agencies and self-hosters needing CWV improvements without app changes or commercial CDN optimization.
- CDN/reverse-proxy, e-commerce, regulated, and air-gapped environments.

## Outcomes

- Maintained NGINX, Apache, and universal proxy optimization.
- Measurable LCP/INP/CLS improvement.
- Lower origin egress/image bandwidth.
- Consistent per-tenant optimization.
- Replace hand-written optimization config with one audited engine.
- Publish reproducible k6/performance evidence.

# A. Compatibility Contract

Laghu covers the legacy functional surface so migration does not lose capability.

## A1. Rewrite levels

`PassThrough`, `CoreFilters`, `OptimizeForBandwidth`, `All`, `Experimental`.

## A2. Image filters

`rewrite_images`, `recompress_images`, `recompress_jpeg`, `recompress_png`, `recompress_webp`, `convert_jpeg_to_progressive`, `convert_jpeg_to_webp`, `convert_png_to_jpeg`, `convert_gif_to_png`, `convert_to_webp_lossless`, `convert_to_webp_animated`, `jpeg_sampling`, `resize_images`, `resize_rendered_image_dimensions`, `resize_mobile_images`, `responsive_images`, `responsive_images_zoom`, `insert_image_dimensions`, `inline_images`, `inline_preview_images`, `dedup_inlined_images`, `lazyload_images`, `sprite_images`, `strip_image_meta_data`, `strip_image_color_profile`, `in_place_optimize_for_browser`.

## A3. CSS filters

`rewrite_css`, `combine_css`, `inline_css`, `outline_css`, `flatten_css_imports`, `inline_import_to_link`, external-font CSS inlining, `move_css_to_head`, `move_css_above_scripts`, `prioritize_critical_css`, `rewrite_style_attributes`, `rewrite_style_attributes_with_url`, `fallback_rewrite_css_urls`.

## A4. JavaScript filters

`rewrite_javascript`, `rewrite_javascript_external`, `rewrite_javascript_inline`, `combine_javascript`, `inline_javascript`, `outline_javascript`, `defer_javascript`, `include_js_source_maps`.

## A5. HTML filters

`add_head`, `combine_heads`, `collapse_whitespace`, `remove_comments`, `remove_quotes`, `elide_attributes`, `convert_meta_tags`, `add_instrumentation`, `hint_preload_subresources`, `insert_dns_prefetch`, `trim_urls`, `pedantic`.

## A6. Cache/URL filters

`extend_cache`, per-type cache extension, `local_storage_cache`, `rewrite_domains`.

## A7. Configuration/operations parity

- Runtime on/off; rewrite levels; enable/disable/forbid filters.
- File cache limits/cleaning/inodes; memory LRU/shared metadata; optional shared cache tier.
- Domain mapping/sharding/proxying; direct file loading.
- `Vary`, forwarded-protocol, allow/disallow URL rules.
- Per-location/server scope and inheritance.
- Statistics/admin/console/history/purge.
- Decision/cache headers and query overrides.
- Purge method/file/query.
- In-place resource optimization.
- Critical-image/CSS beaconing.
- Experiment/A/B framework.

## A8. Legacy pain points to eliminate

Dead upstream/CVE response; exact-version/binary build fragility; stale codecs; no modern CWV; loopback refetch; opaque memory/failures; weak observability; config sprawl; no modern Early Hints/protocol story.

# B. Laghu Expansion

## B1. Migration/parity

Full functional parity with Part A. One-way `laghu migrate` converts legacy config; runtime accepts only `laghu` directives.

## B2. Image pipeline

- AVIF negotiation; flag-gated JPEG XL; modern lossy/lossless/animated WebP.
- Perceptual quality targeting via SSIMULACRA2/DSSIM.
- Content-aware presets; optional denoise-before-encode.
- Mobile/tablet/desktop, 1x/2x, `Save-Data`, and client-hint variants.
- SVG optimization; optional simple raster→vector.
- LQIP; automatic dimensions/aspect ratio.
- LCP `fetchpriority=high`; safe lazy loading.
- Large animated GIF → MP4/WebM option.

## B3. Core Web Vitals

- **LCP:** detect/preload/prioritize; Early Hints; never lazy-load LCP.
- **CLS:** reserve image/ad/embed/font space; dimensions/aspect ratio; font-display/preload.
- **INP:** defer non-critical JS; safe task splitting; interaction-delayed third parties.
- **TTFB:** HTML micro-cache, `304`, stale-while-revalidate.
- Per-template profiles by DOM-structure hash.
- Optional headless-Chrome analysis for critical CSS/LCP/rendered dimensions; non-blocking heuristic fallback.

## B4. Delivery/protocols

- `103 Early Hints`, preconnect, DNS-prefetch.
- HTTP/2 and HTTP/3/QUIC-safe transforms.
- Pre-compressed Brotli/Gzip variants.
- Immutable content-hashed resources and long TTLs.
- Capability-aware cache keys / safe `Vary`.
- CDN-safe headers and origin-shield mode.

## B5. Safety/correctness/security

- CSP-safe transforms; nonce/`strict-dynamic` awareness.
- Never-larger variants.
- Idempotent/reversible; original recoverable.
- Respect `no-store`/`private`; auth/API bypass defaults.
- SSRF-safe fetch defaults.
- Dark-mode-safe critical CSS.
- Deterministic fleet-stable output.
- Fail open to original content.

## B6. Observability/operations

Prometheus metrics, structured JSON logs, OpenTelemetry, health/readiness, opt-in RUM for LCP/INP/CLS, Grafana dashboard, `laghu doctor`.

## B7. Usability

- CLI: `status`, `purge <url>`, `doctor`, `bench`, `explain <url>`, `migrate`.
- Presets: `safe`, `balanced`, `aggressive`, `ecommerce`, `blog`, `static`.
- Config validation on load.
- Web console: stats, explain, purge, before/after bytes/CWV, filter controls.
- Query debug: `?laghu=off`, `?laghu=explain`, `?laghuFilters=...`.
- Dry-run/preview mode.

## B8. Migration map

| Legacy input | Laghu output |
| --- | --- |
| `pagespeed on;` | `laghu on;` |
| `pagespeed RewriteLevel CoreFilters;` | `laghu preset balanced;` |
| `pagespeed EnableFilters rewrite_images,...;` | `laghu enable image,css,js,...;` |
| `pagespeed FileCachePath /var/cache;` | `laghu cache_path /var/cache/laghu;` |
| `pagespeed Disallow "*/admin/*";` | `laghu disallow /admin/;` |
| `/pagespeed_admin`, `/pagespeed_console` | `/laghu/console`, `/metrics` |
| `?PageSpeedFilters=` | `?laghuFilters=` |
| IPRO | `laghu ipro on;` / on by default |

# C. Performance Rail

Purpose: reproducible correctness/performance evidence and regression gating across all optimizations, content types, sizes, and deployment targets.

## C1. Baselines

1. Plain NGINX.
2. Frozen newest runnable legacy `ngx_pagespeed` + compatible NGINX.
3. Laghu on current NGINX.

Compatibility job finds the newest legacy build passing runtime smoke; freeze it as a reproducible image. If none succeeds, use plain NGINX with equivalent hand-written optimization as fallback baseline.

## C2. Deterministic corpus

- **HTML:** 1 KB, 10 KB, 100 KB, 1 MB, 5 MB; sparse, asset-heavy, inline-CSS/JS-heavy, comments, deep DOM.
- **CSS:** 1 KB, 10 KB, 100 KB, 1 MB, 2 MB; imports, URLs, unused rules, pre-minified, framework-scale.
- **JS:** 1 KB–2 MB; minified, source-mapped, module/classic, blocking/deferred.
- **Images:** JPEG/PNG/static+animated GIF/WebP/AVIF/SVG; 100/480/768/1440/4K/8000 px; 5 KB–10 MB; photo/screenshot/flat/noisy/transparent/logo/icon.
- **Capabilities:** WebP/AVIF, `Save-Data`, 1x/2x DPR, mobile/tablet/desktop hints.
- **Fonts:** WOFF2/WOFF, subset/full, icon fonts.
- **Edges:** empty, `no-store`, hashed URLs, huge query strings, malformed HTML, mixed content, non-UTF-8.

## C3. k6 driver

Scenarios: cold, warm, 1/10/50/100/500/1000 VUs, ramp/soak, same-URL cache storm, capability fanout, realistic mixed page.

Metrics: TTFB, p50/p90/p95/p99, throughput, errors, wire bytes/savings, cache state, transforms fired.

Correctness gates:
- Valid HTML/CSS/JS and decodable requested image formats.
- Optimized ≤ original.
- Excluded/auth/private/no-store paths untouched.
- Expected CLS-critical attributes present.
- Expected transforms actually fire.

## C4. Server/quality/CWV instrumentation

- cgroup/cAdvisor CPU, RSS, CPU per optimized byte.
- Optimizer latency histograms.
- SSIMULACRA2/DSSIM image quality.
- Lighthouse/CDP LCP, INP, CLS, TBT.

## C5. Results/gates

For each optimization × content type × size × scenario report target, dimensions, scenario, bytes, TTFB/p95, throughput, CPU/RSS, quality, CWV delta, verdict.

Emit machine-readable JSON + rendered HTML/Grafana output. CI fails regressions beyond tolerance versus plain NGINX, or core-filter regressions versus legacy baseline.

## C6. Reproducibility

Containerized one-command rail (`laghu bench --full` / `make bench`), seeded corpus, release/scheduled runs, historical trends, pinned baseline images, recorded hardware profile.

# D. Architecture

- Shared server-neutral HTTP transaction contract; adapters own transport only.
- Lightweight NGINX/Apache/proxy adapters; heavy transforms out-of-process (e.g. `laghu-libvips`).
- First hit serves original; validated variants publish asynchronously for later hits.
- Shared bounded cache/queue/catalogs with LRU and per-URL purge.
- Standalone `laghu` owns proxy transport/origin forwarding; `laghu-libvips` never proxies.
- Fail open everywhere.
- Deterministic content-hashed variants for fleet-safe long-TTL caching.

# E. Licensing/Support

MIT. No open-core, paid tier, SaaS dependency, license key, feature gate, nag screen, or crippled community edition. Optional professional support does not change product functionality.

# F. Distribution

- First-class packages: `ngx-laghu`, `mod-laghu`, `laghu`; `laghu-libvips` internal.
- Prebuilt supported NGINX/Apache adapters and ABI-bound packages.
- deb, rpm, Homebrew.
- NGINX, Apache, standalone containers; Helm chart.
- NGINX, Apache 2.4, OpenResty, Angie, freenginx.
- LTS branches, CVE/security-response policy.
- Migration guides from legacy modules/CDN optimizers.

# G. Positioning Summary

> Laghu is one free optimization engine deployed as `ngx-laghu`, `mod-laghu`, or the self-hosted `laghu` reverse proxy, with shared behavior proven across all three surfaces.
