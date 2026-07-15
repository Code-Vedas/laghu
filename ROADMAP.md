# Laghu Progress Tracker

This is a temporary internal execution tracker. It preserves the complete work
inventory while Laghu is built and will be removed when it is no longer needed.
It is not public product documentation and public docs or code must not depend
on it.

## Tracking Rules

- `[x]` means the capability is implemented and backed by executable validation.
- `[ ]` means the capability is pending, including partially scaffolded work.
- Never remove an unchecked item to make progress appear complete.
- Keep items granular: a parser, interface, or skeleton does not complete the
  runtime behavior it will eventually support.
- Check an item only after its implementation, tests, documentation, and relevant
  build, runtime, correctness, or performance validation are complete.
- If later work invalidates the evidence, return the item to `[ ]`.
- Scope additions must be added here before implementation begins.

## 1. Repository Bootstrap

- [x] Establish canonical `libs/`, `modules/`, `docs/`, `examples/`, `scripts/`,
  and `.github/` ownership boundaries.
- [x] Add the MIT license, security policy, contribution guide, code of conduct,
  changelog, issue forms, pull request template, Dependabot, CodeQL, release
  drafting, and documentation deployment configuration.
- [x] Add root build, test, lint, module-build, module-smoke, documentation, and
  Docker validation entrypoints.
- [x] Add a C11 server-independent core library with direct CTest coverage.
- [x] Add a native NGINX HTTP auxiliary-filter module skeleton.
- [x] Build and runtime-smoke the module against pinned stable and mainline NGINX.
- [x] Add a standalone Just the Docs site with custom includes and light/dark
  theme assets.
- [x] Add the one-command `scripts/run-all` repository validation lane.

## 2. Product and Runtime Contract

- [x] Use only the `laghu` runtime namespace, `X-Laghu` response namespace, and
  `/laghu/...` endpoint namespace.
- [x] Keep legacy runtime names out of shipped directives, headers, endpoints,
  packages, and images.
- [x] Keep the optimization policy library independent of NGINX types.
- [x] Support `laghu on|off` with `http`, `server`, and `location` inheritance.
- [x] Parse the `safe`, `balanced`, `aggressive`, `ecommerce`, `blog`, and
  `static` preset identifiers and reject invalid values during configuration.
- [x] Resolve every preset to its documented filter-family and safety policy
  set without claiming that pending filters execute.
- [x] Preserve the original response body in the initial filter skeleton.
- [x] Conservatively bypass unsupported statuses, authenticated requests,
  private responses, and unsupported content types.
- [x] Exclude API paths by default with configurable overrides.
- [x] Validate fail-open behavior for the completed pass-through delivery path
  and establish the mandatory fallback contract for future components.
- [x] Keep the completed pass-through path idempotent and reversible, and
  preserve a borrowed view of the original in every candidate-selection
  result.
- [x] Produce versioned, deterministic, fleet-stable variant keys from original
  content and resolved policy.
- [x] Establish a candidate-selection gate that accepts only validated,
  non-identical output that is strictly smaller than the original.

Completion evidence: the server-independent core resolves all preset policies,
applies API-path eligibility, produces versioned SHA-256 variant keys, and
preserves an original view through its candidate-selection gate. Core unit
tests and stable/mainline NGINX smoke tests cover inheritance, invalid
configuration, decision precedence, API overrides, boundary matching, safety
precedence, and unchanged response delivery. No optimizer, worker, or cache is
claimed as implemented by this milestone; their end-to-end guarantees remain
pending in their corresponding roadmap sections.

## 3. Compatibility-Parity Inventory

- [ ] Complete functional parity across every rewrite level, filter,
  configuration surface, cache behavior, administration surface, and purge path
  in this inventory.

### 3.1 Rewrite Levels

- [x] `PassThrough`: select an empty policy while keeping the module loaded and
  the original response unchanged.
- [x] `CoreFilters`: select the balanced, recommended-default filter-family and
  safety policy.
- [x] `OptimizeForBandwidth`: select byte-saving families without structural,
  inline, combine, critical-CSS, or script-order changes.
- [x] `All` / `Experimental`: select every current family, with experimental
  permission enabled only by the explicit experimental level.

Completion evidence: the native `laghu rewrite_level` directive parses,
inherits, and resolves passthrough, core, bandwidth, all, and experimental
policies. Core tests cover exact masks, safety permissions, selector conflicts,
cross-kind inheritance, passthrough enforcement, and variant-key versioning.
Stable/mainline NGINX smoke tests cover accepted values, invalid/duplicate/
conflicting configuration, child overrides, decision headers, and unchanged
response delivery. Actual filter execution remains tracked below, so the
Section 3 full-parity item remains pending.

### 3.2 Image Filters

- [ ] `rewrite_images`: master image optimizer and sub-filter coordination.
- [ ] `recompress_images`: lossless recompression.
- [ ] `recompress_jpeg`: JPEG-specific recompression.
- [ ] `recompress_png`: PNG-specific recompression.
- [ ] `recompress_webp`: WebP recompression.
- [ ] `convert_jpeg_to_progressive`: baseline-to-progressive JPEG conversion.
- [ ] `convert_jpeg_to_webp`: capable-client JPEG-to-WebP conversion.
- [ ] `convert_png_to_jpeg`: opaque PNG-to-JPEG conversion.
- [ ] `convert_gif_to_png`: static GIF-to-PNG conversion.
- [ ] `convert_to_webp_lossless`: PNG/GIF-to-lossless-WebP conversion.
- [ ] `convert_to_webp_animated`: animated GIF-to-animated-WebP conversion.
- [ ] `jpeg_sampling`: JPEG 4:2:0 chroma subsampling.
- [ ] `resize_images`: resize from image element dimensions.
- [ ] `resize_rendered_image_dimensions`: resize from actual rendered dimensions.
- [ ] `resize_mobile_images`: mobile-specific smaller variants.
- [ ] `responsive_images`: generate multi-resolution `srcset` variants.
- [ ] `responsive_images_zoom`: zoom-aware responsive variants.
- [ ] `insert_image_dimensions`: inject image dimensions to prevent layout shift.
- [ ] `inline_images`: inline small images as data URIs.
- [ ] `inline_preview_images`: generate and inline low-quality previews.
- [ ] `dedup_inlined_images`: deduplicate repeated inline images.
- [ ] `lazyload_images`: defer eligible offscreen images.
- [ ] `sprite_images`: combine CSS background images into sprites.
- [ ] `strip_image_meta_data`: remove EXIF and other metadata.
- [ ] `strip_image_color_profile`: remove eligible ICC profiles.
- [ ] `in_place_optimize_for_browser`: optimize directly requested resources for
  client capabilities.

### 3.3 CSS Filters

- [ ] `rewrite_css`: minify CSS and rewrite embedded URLs.
- [ ] `combine_css`: combine compatible stylesheets.
- [ ] `inline_css`: inline eligible small external stylesheets.
- [ ] `outline_css`: externalize eligible large inline style blocks.
- [ ] `flatten_css_imports`: flatten compatible `@import` chains.
- [ ] `inline_import_to_link`: convert `@import` rules to links.
- [ ] `inline_google_font_css`: inline eligible Google Fonts CSS.
- [ ] `move_css_to_head`: move stylesheet links into the document head.
- [ ] `move_css_above_scripts`: move CSS above script elements.
- [ ] `prioritize_critical_css`: inline critical CSS and defer the remainder.
- [ ] `rewrite_style_attributes`: rewrite inline style attributes.
- [ ] `rewrite_style_attributes_with_url`: rewrite style attributes containing
  `url()`.
- [ ] `fallback_rewrite_css_urls`: safely rewrite URLs in otherwise unparseable
  CSS.

### 3.4 JavaScript Filters

- [ ] `rewrite_javascript`: master JavaScript minifier.
- [ ] `rewrite_javascript_external`: minify external JavaScript.
- [ ] `rewrite_javascript_inline`: minify inline JavaScript.
- [ ] `combine_javascript`: combine compatible JavaScript resources.
- [ ] `inline_javascript`: inline eligible small external scripts.
- [ ] `outline_javascript`: externalize eligible large inline scripts.
- [ ] `defer_javascript`: safely defer non-critical execution.
- [ ] `include_js_source_maps`: preserve or emit source-map references.

### 3.5 HTML Filters

- [ ] `add_head`: add a missing document head.
- [ ] `combine_heads`: merge multiple head elements.
- [ ] `collapse_whitespace`: remove safe excess whitespace.
- [ ] `remove_comments`: strip eligible HTML comments.
- [ ] `remove_quotes`: remove unnecessary attribute quotes.
- [ ] `elide_attributes`: remove default-value attributes.
- [ ] `convert_meta_tags`: convert eligible HTTP-equivalent meta tags to headers.
- [ ] `add_instrumentation`: inject opt-in real-user measurement instrumentation.
- [ ] `hint_preload_subresources`: add preload link hints.
- [ ] `insert_dns_prefetch`: add DNS-prefetch hints for third parties.
- [ ] `trim_urls`: shorten URLs relative to the document base.
- [ ] `pedantic`: emit type attributes required by legacy HTML validity modes.

### 3.6 Caching and URL Filters

- [ ] `extend_cache`: content-hashed URLs with safe long-lived browser caching.
- [ ] `extend_cache_css`: CSS-specific cache extension.
- [ ] `extend_cache_scripts`: script-specific cache extension.
- [ ] `extend_cache_images`: image-specific cache extension.
- [ ] `extend_cache_pdfs`: PDF-specific cache extension.
- [ ] `local_storage_cache`: client-side caching for eligible inline resources.
- [ ] `rewrite_domains`: resource domain mapping and rewriting.

### 3.7 Configuration and Operations Parity

- [x] Runtime on/off configuration and hierarchical inheritance.
- [ ] Unplugged runtime mode.
- [x] Rewrite-level selection.
- [ ] Enable, disable, and forbid individual filters.
- [ ] File-backed cache path, size, cleaning interval, and inode limits
  (`FileCachePath` and related controls in migration input).
- [ ] In-memory LRU cache and shared-memory metadata cache.
- [ ] Optional memcached cache tier.
- [ ] Domain mapping, sharding, proxying, and rewrite-domain configuration
  (`Domain`, `MapRewriteDomain`, `ShardDomain`, and `MapProxyDomain` in
  migration input).
- [ ] Direct file loading that avoids loopback origin fetches (`LoadFromFile` in
  migration input).
- [ ] Configurable `Vary` and forwarded-protocol handling (`RespectVary` and
  `RespectXForwardedProto` in migration input).
- [ ] Allow/disallow URL wildcard filtering (`AllowResources` and `Disallow` in
  migration input).
- [x] Per-location and per-server configuration scope and inheritance.
- [ ] Statistics, administration, message history, console, and cache-purge UI,
  including migration from the legacy administration/statistics/console paths.
- [x] Basic `X-Laghu` pass/bypass decision response header.
- [ ] Cache-state and transform-decision response headers.
- [ ] Per-request query-string filter overrides.
- [ ] Purge method, cache flush file, and query-driven purge (`PurgeMethod` and
  purge-query migration inputs).
- [ ] In-place resource optimization.
- [ ] Client beaconing for critical-image and critical-CSS discovery.
- [ ] Experiment framework for controlled filter-set rollout.

### 3.8 Legacy Pain-Point Non-Regression

- [ ] Maintain active releases, current supported NGINX compatibility, security
  response, and CVE handling.
- [ ] Eliminate opaque binary-library dependencies and exact-version manual
  build breakage from supported installation paths.
- [ ] Keep modern image encoders and protocol behavior maintained.
- [ ] Make Core Web Vitals first-class optimization targets.
- [ ] Avoid loopback resource re-fetch where direct or worker-side access is
  available.
- [ ] Bound and expose memory use, cache growth, and optimizer failure reasons.
- [ ] Replace text-only operational surfaces with metrics, structured logs, and
  traces.
- [ ] Keep configuration understandable through presets, validation, and
  explainability despite the complete filter surface.
- [ ] Use Early Hints and modern resource hints instead of HTTP/2 push.

## 4. Modern Optimization Features

### 4.1 Image Pipeline

- [ ] AVIF encoding and `Accept`-based negotiation.
- [ ] Flag-gated JPEG XL encoding when client support warrants it.
- [ ] Modern lossy, lossless, and animated WebP encoding.
- [ ] Perceptual quality targeting with SSIMULACRA2 or DSSIM.
- [ ] Photo, screenshot, illustration, and flat-color classification with
  content-aware presets.
- [ ] Denoise-before-encode for suitable noisy sources.
- [ ] Mobile, tablet, and desktop viewport-width variants.
- [ ] 1x and 2x pixel-density variants.
- [ ] Lower-quality `Save-Data` variants.
- [ ] Client-hint-aware selection using `Sec-CH-DPR` and
  `Sec-CH-Viewport-Width`.
- [ ] SVG optimization and optional simple raster-to-vector conversion.
- [ ] LQIP and blur-placeholder generation.
- [ ] Automatic width, height, and aspect-ratio injection.
- [ ] LCP-image `fetchpriority=high` with safe below-fold lazy loading.
- [ ] Large animated GIF conversion to MP4/WebM video markup.

### 4.2 Core Web Vitals

- [ ] Detect, preload, prioritize, and exclude the LCP element from lazy loading.
- [ ] Reserve layout space for images, ads, embeds, and fonts to reduce CLS.
- [ ] Add appropriate font-display behavior and font preloads.
- [ ] Defer non-critical JavaScript, safely split long tasks, and delay eligible
  third-party scripts until interaction to improve INP.
- [ ] Add HTML micro-caching, conditional revalidation (`304`), and
  stale-while-revalidate to improve TTFB.
- [ ] Learn and apply optimization profiles by DOM-template hash.
- [ ] Add an optional headless-Chrome analysis tier for critical CSS, viewport
  state, LCP, and rendered image dimensions.
- [ ] Provide a non-blocking heuristic fallback when browser analysis is absent
  or fails.

### 4.3 Delivery and Protocols

- [ ] Emit `103 Early Hints` for eligible preload and preconnect targets.
- [ ] Inject preconnect and DNS-prefetch hints for detected third-party origins.
- [ ] Validate all transforms and caching behavior under HTTP/2.
- [ ] Validate all transforms and caching behavior under HTTP/3/QUIC.
- [ ] Store Brotli and Gzip pre-compressed text variants at optimization time.
- [ ] Serve immutable, content-hashed resources with safe long TTLs.
- [ ] Key cached variants by `Vary` inputs and a bounded capability mask.
- [ ] Emit CDN-safe headers and support origin-shield operation.

### 4.4 Safety and Security

- [ ] Make transforms Content-Security-Policy-safe, including nonce and
  `strict-dynamic` detection and unsafe-transform auto-disable.
- [ ] Respect `no-store` and `private` throughout every cache and transform path.
- [x] Bypass authenticated requests in the initial eligibility policy.
- [ ] Make all outbound resource fetching SSRF-safe with private and loopback
  access disabled by default.
- [ ] Document and implement dark-mode-safe critical-CSS behavior.
- [ ] Bound content size, memory, optimization time, cache growth, and variant
  fanout.

### 4.5 Observability and Operations

- [ ] Expose Prometheus metrics for cache behavior, variants, bytes saved,
  per-type latency, errors, and cache size.
- [ ] Emit structured JSON logs with per-request optimization decisions.
- [ ] Emit OpenTelemetry traces across the optimization path.
- [ ] Add health and readiness endpoints.
- [ ] Add an opt-in RUM beacon for LCP, INP, and CLS feedback by template.
- [ ] Ship a Grafana dashboard.
- [ ] Implement `laghu doctor` for configuration, NGINX compatibility, cache,
  permissions, and operational diagnostics.

### 4.6 CLI and Usability

- [ ] Implement `laghu status`.
- [ ] Implement `laghu purge <url>`.
- [ ] Implement `laghu doctor`.
- [ ] Implement `laghu bench`.
- [ ] Implement `laghu explain <url>`.
- [ ] Implement `laghu migrate` as a one-way legacy configuration converter.
- [ ] Complete the documented `safe`, `balanced`, `aggressive`, `ecommerce`,
  `blog`, and `static` preset behavior.
- [x] Reject unsupported preset values during NGINX configuration loading.
- [ ] Validate every directive and invalid cross-directive combination during
  configuration loading with actionable errors.
- [ ] Add a web console with statistics, per-URL explanations, purge,
  before/after comparisons, and filter controls.
- [ ] Add `?laghu=off`, `?laghu=explain`, and `?laghuFilters=...` request
  debugging.
- [ ] Add dry-run/preview mode that reports changes without serving them.

### 4.7 Migration Converter Coverage

- [ ] Convert legacy runtime on/off configuration.
- [ ] Convert legacy rewrite levels to Laghu presets.
- [ ] Convert legacy enabled-filter lists to Laghu filter groups.
- [ ] Convert legacy cache paths to Laghu cache paths.
- [ ] Convert legacy disallow rules to Laghu disallow rules.
- [ ] Convert legacy administration and console endpoints.
- [ ] Convert legacy query-filter overrides.
- [ ] Convert legacy in-place resource optimization configuration.
- [ ] Keep all legacy tokens isolated to the converter input parser and
  migration/background documentation.

## 5. Packaging and Compatibility

- [x] Compile the dynamic module against pinned current stable and mainline
  NGINX source versions in CI.
- [ ] Publish prebuilt dynamic modules for every supported NGINX release.
- [ ] Publish deb packages.
- [ ] Publish rpm packages.
- [ ] Publish production Docker images.
- [ ] Publish a Helm chart.
- [ ] Add tested OpenResty compatibility.
- [ ] Add tested Angie compatibility.
- [ ] Add tested freenginx compatibility.
- [ ] Add a standalone sidecar/reverse-proxy distribution.
- [ ] Maintain LTS branches with a published CVE and security-response process.
- [ ] Publish a complete migration guide for legacy module and CDN-optimizer
  users.

## 6. Performance and Correctness Rail

### 6.1 Baseline Substrates

- [ ] Add a compatibility job that tries legacy module builds from newest to
  older candidate NGINX releases.
- [ ] Record the highest successful release as `baseline_nginx_version` after a
  runtime smoke test.
- [ ] Freeze and archive the successful legacy baseline Docker image.
- [ ] Provide a plain-NGINX fallback baseline with equivalent hand-written
  compression, cache-header, and format-selection configuration.
- [ ] Build the product comparison image on a pinned current NGINX release.
- [ ] Run every benchmark against plain NGINX, the reproducible legacy baseline,
  and Laghu on current NGINX.

### 6.2 Deterministic Content Corpus

- [ ] Generate HTML at 1 KB, 10 KB, 100 KB, 1 MB, and 5 MB.
- [ ] Cover sparse, asset-heavy, inline-CSS-heavy, inline-JS-heavy,
  comment-heavy, and deeply nested HTML.
- [ ] Generate CSS at 1 KB, 10 KB, 100 KB, 1 MB, and 2 MB.
- [ ] Cover import-heavy, URL-heavy, unused-rule-heavy, already-minified, and
  framework-scale CSS.
- [ ] Generate JavaScript from 1 KB through 2 MB.
- [ ] Cover already-minified, source-mapped, module, classic, render-blocking,
  and deferred JavaScript.
- [ ] Generate JPEG, PNG, static/animated GIF, WebP, AVIF, and SVG sources.
- [ ] Cover 100 px, 480 px, 768 px, 1440 px, 4K, and oversized 8000 px images.
- [ ] Cover image payloads from 5 KB thumbnails through the 10 MB processing cap.
- [ ] Cover photo, screenshot, flat illustration, noisy, transparent, logo, and
  icon image classes.
- [ ] Cover WebP/AVIF acceptance, `Save-Data`, 1x/2x DPR, and mobile/tablet/
  desktop viewport capability permutations.
- [ ] Generate WOFF2, WOFF, subsettable, full, and icon-font cases.
- [ ] Cover empty responses, `no-store`, already-hashed URLs, huge query strings,
  malformed HTML, mixed content, and non-UTF-8 inputs.

### 6.3 k6 Load and Correctness Driver

- [ ] Add cold-cache scenarios for first-hit optimization latency and TTFB.
- [ ] Add warm-cache scenarios for steady-state delivery latency.
- [ ] Sweep 1, 10, 50, 100, 500, and 1000 virtual users.
- [ ] Add sustained ramp and soak scenarios for memory growth and cache thrash.
- [ ] Add same-URL cache-storm scenarios that verify optimization deduplication.
- [ ] Add capability variant-fanout scenarios and cache-growth checks.
- [ ] Add realistic mixed page loads with HTML, images, CSS, and JavaScript.
- [ ] Capture TTFB, p50/p90/p95/p99 response time, throughput, and error rate.
- [ ] Capture original/optimized wire bytes and byte-savings percentage.
- [ ] Capture cache hit/miss ratios and assert the expected cache state.
- [ ] Capture and assert which transforms fired.
- [ ] Validate parsed HTML and executable CSS/JavaScript behavior.
- [ ] Validate that image variants decode and match requested formats.
- [ ] Enforce the never-larger guarantee.
- [ ] Assert that excluded paths and responses remain untouched.
- [ ] Assert expected CLS-critical dimensions and attributes.

### 6.4 Resource, Quality, and CWV Instrumentation

- [ ] Capture cAdvisor/cgroup CPU, RSS, and CPU per optimized byte.
- [ ] Capture optimizer latency histograms from product metrics.
- [ ] Calculate SSIMULACRA2 or DSSIM for every optimized image.
- [ ] Run Lighthouse/CDP measurements for LCP, INP, CLS, and TBT on
  representative pages.

### 6.5 Results and Regression Gates

- [ ] Emit a result cell for every optimization, content type, size, and scenario.
- [ ] Report target, content dimensions, scenario, bytes, TTFB, p95, throughput,
  CPU, RSS, quality, CWV delta, and verdict.
- [ ] Produce machine-readable JSON results.
- [ ] Produce a rendered HTML report.
- [ ] Produce Grafana snapshots.
- [ ] Fail CI when Laghu regresses beyond tolerance against plain NGINX.
- [ ] Fail CI when a core filter regresses against the legacy baseline.

### 6.6 Reproducibility and Automation

- [ ] Run the full containerized rail with `laghu bench --full` and `make bench`.
- [ ] Seed the corpus generator for comparable repeated runs.
- [ ] Run the rail for every release and on a schedule.
- [ ] Store historical results and trend lines.
- [ ] Pin and archive all baseline images.
- [ ] Record CPU model, core count, memory, and other relevant hardware context.

## 7. Production Architecture

- [ ] Add a two-process architecture: lightweight NGINX interceptor plus
  asynchronous out-of-process optimization worker.
- [ ] Serve the original on first hit while an optimized variant is generated
  without blocking the NGINX event loop.
- [ ] Add a shared memory-mapped cache readable by NGINX and the worker.
- [ ] Add bounded LRU eviction and per-URL purge.
- [ ] Reuse the optimization worker in standalone sidecar/reverse-proxy mode.
- [ ] Make worker, socket, cache, and optimization failures fail open.
- [ ] Generate deterministic content-hashed variant URLs for fleet-safe caching.

## 8. Open Source and Support

- [x] License the repository under MIT.
- [ ] Ship every completed optimization, CLI command, benchmark, and package
  without paid tiers, license keys, open-core boundaries, or feature gating.
- [x] Publish a low-friction professional-support page.
- [ ] Keep support optional and free of nag screens, dark patterns, or crippled
  community functionality as the product grows.
- [ ] Document setup, tuning, migration, incident, and retainer support paths.

## 9. Outcome Evidence

- [ ] Demonstrate a maintained native optimization path on supported NGINX.
- [ ] Demonstrate measurable LCP, INP, and CLS improvements by template.
- [ ] Demonstrate reduced origin egress and image bandwidth without application
  changes.
- [ ] Demonstrate safe per-tenant operation for shared hosting environments.
- [ ] Demonstrate that the module replaces equivalent hand-written optimization
  configuration without losing correctness.
- [ ] Publish evidence for every optimization across content types and sizes.
