# Laghu Progress Tracker

## Tracking Rules

## 1. Repository Bootstrap
- [x] Establish canonical `libs/`, `modules/`, `docs/`, `examples/`, `scripts/`, and `.github/` ownership boundaries.
- [x] Add the MIT license, security policy, contribution guide, code of conduct, changelog, issue forms, pull request template, Dependabot, CodeQL, release drafting, and documentation deployment configuration.
- [x] Add root build, test, lint, module-build, module-smoke, documentation, and Docker validation entrypoints.
- [x] Add a C11 server-independent core library with direct CTest coverage.
- [x] Add server-independent image and queue/cache runtime libraries plus the out-of-process `laghu-libvips` service.
- [x] Add a native NGINX HTTP auxiliary-filter module skeleton.
- [x] Add a first-class Apache 2.4 output-filter module using APR bucket brigades.
- [x] Add a first-class self-hosted `laghu` reverse-proxy executable that can optimize responses from any HTTP origin without NGINX or Apache integration.
- [x] Build and runtime-smoke the module against pinned stable and mainline NGINX.
- [x] Build and runtime-smoke both adapters through cold-original and warm-variant image delivery.
- [x] Add a standalone Just the Docs site with custom includes and light/dark theme assets.
- [x] Add the one-command `scripts/run-all` repository validation lane.

## 2. Product and Runtime Contract
- [x] Use only the `laghu` runtime namespace, `X-Laghu` response namespace, and `/laghu/...` endpoint namespace.
- [x] Keep legacy runtime names out of shipped directives, headers, endpoints, packages, and images.
- [x] Keep the optimization policy library independent of NGINX types.
- [x] Support `laghu on|off` with `http`, `server`, and `location` inheritance.
- [x] Parse the `safe`, `balanced`, `aggressive`, `ecommerce`, `blog`, and `static` preset identifiers and reject invalid values during configuration.
- [x] Resolve every preset to its documented filter-family and safety policy set without claiming that pending filters execute.
- [x] Preserve the original response body in the initial filter skeleton.
- [x] Conservatively bypass unsupported statuses, authenticated requests, private responses, and unsupported content types.
- [x] Exclude API paths by default with configurable overrides.
- [x] Validate fail-open behavior for the completed pass-through delivery path and establish the mandatory fallback contract for future components.
- [x] Keep the completed pass-through path idempotent and reversible, and preserve a borrowed view of the original in every candidate-selection result.
- [x] Produce versioned, deterministic, fleet-stable variant keys from original content and resolved policy.
- [x] Establish a candidate-selection gate that accepts only validated, non-identical output that is strictly smaller than the original.

## 3. Compatibility-Parity Inventory
- [ ] Complete functional parity across every rewrite level, filter, configuration surface, cache behavior, administration surface, and purge path in this inventory.

### 3.0 Three-Surface Delivery Foundation
- [x] Inventory duplicated native-adapter orchestration and assign request classification, policy invocation, warm lookup, derivation, header planning, body selection, and fail-open behavior to a shared engine while retaining capture and transport mechanics in each adapter.
- [x] Add a bounded, versioned `laghu_http_transaction` API in a shared library without NGINX, APR, socket, TLS, or event-loop types.
- [x] Add table-driven conformance fixtures that feed normalized HTTP transactions through the shared engine and assert decisions, body bytes, header operations, dependency ETags, cache keys, and failure results.
- [x] Refactor `ngx-laghu` and `mod-laghu` into transport/configuration adapters over that contract without regressing NGINX chains or Apache brigades.
- [x] Implement the standalone `laghu` reverse proxy with bounded HTTP/1.1 origin forwarding; HTTP/2 and HTTP/3 remain separately tracked transport milestones.
- [x] Validate cold-original and warm-derived HTML, CSS, image, internal asset, API/auth/private bypass, worker-loss, cache-corruption, timeout, disconnect, and malformed-origin behavior through all three surfaces.

### 3.1 Rewrite Levels
- [x] `PassThrough`: select an empty policy while keeping the module loaded and the original response unchanged.
- [x] `CoreFilters`: select the balanced, recommended-default filter-family and safety policy.
- [x] `OptimizeForBandwidth`: select byte-saving families without structural, inline, combine, critical-CSS, or script-order changes.
- [x] `All` / `Experimental`: select every current family, with experimental permission enabled only by the explicit experimental level.

### 3.2 Image Filters
- [x] `rewrite_images`: master image optimizer and sub-filter coordination.
- [x] `recompress_images`: lossless recompression.
- [x] `recompress_jpeg`: JPEG-specific recompression.
- [x] `recompress_png`: PNG-specific recompression.
- [x] `recompress_webp`: WebP recompression.
- [x] `convert_jpeg_to_progressive`: baseline-to-progressive JPEG conversion.
- [x] `convert_jpeg_to_webp`: capable-client JPEG-to-WebP conversion.
- [x] `convert_png_to_jpeg`: opaque PNG-to-JPEG conversion.
- [x] `convert_gif_to_png`: static GIF-to-PNG conversion.
- [x] `convert_to_webp_lossless`: PNG/GIF-to-lossless-WebP conversion.
- [x] `convert_to_webp_animated`: animated GIF-to-animated-WebP conversion.
- [x] `jpeg_sampling`: JPEG 4:2:0 chroma subsampling.
- [x] `resize_images`: resize from image element dimensions.
- [x] `resize_rendered_image_dimensions`: resize from actual rendered dimensions.
- [x] `resize_mobile_images`: mobile-specific smaller variants.
- [x] `responsive_images`: generate multi-resolution `srcset` variants.
- [x] `responsive_images_zoom`: zoom-aware responsive variants.
- [x] `insert_image_dimensions`: inject image dimensions to prevent layout shift.
- [x] `inline_images`: inline small images as data URIs.
- [x] `inline_preview_images`: generate and inline low-quality previews.
- [x] `dedup_inlined_images`: deduplicate repeated inline images.
- [x] `lazyload_images`: defer eligible offscreen images.
- [x] `sprite_images`: combine eligible CSS background images into a deterministic lossless PNG sprite.
- [x] `strip_image_meta_data`: remove EXIF and other metadata.
- [x] `strip_image_color_profile`: remove eligible ICC profiles.
- [x] `in_place_optimize_for_browser`: optimize directly requested resources for client capabilities.

### 3.3 CSS Filters
- [x] `rewrite_css`: safely minify CSS and rewrite ready same-format image URLs.
- [x] `combine_css`: combine compatible adjacent stylesheets.
- [x] `inline_css`: inline eligible small external stylesheets.
- [x] `outline_css`: externalize eligible large inline style blocks.
- [x] `flatten_css_imports`: flatten compatible `@import` chains.
- [x] `inline_import_to_link`: convert `@import` rules to links.
- [x] Provider-configurable external font CSS inlining: inline eligible provider stylesheets; Google Fonts and Fontsource CDN are the initial installed providers.
- [x] `move_css_to_head`: move stylesheet links into the document head.
- [x] `move_css_above_scripts`: move CSS above script elements.
- [x] `prioritize_critical_css`: inline critical CSS and defer the remainder.
- [x] `rewrite_style_attributes`: rewrite inline style attributes.
- [x] `rewrite_style_attributes_with_url`: rewrite style attributes containing `url()`.
- [x] `fallback_rewrite_css_urls`: safely rewrite URLs in otherwise unparseable CSS.

### 3.4 JavaScript Filters
- [x] `rewrite_javascript`: master JavaScript minifier.
- [x] `rewrite_javascript_external`: minify external JavaScript.
- [x] `rewrite_javascript_inline`: minify inline JavaScript.
- [x] `combine_javascript`: combine compatible JavaScript resources.
- [x] `inline_javascript`: inline eligible small external scripts.
- [x] `outline_javascript`: externalize eligible large inline scripts.
- [x] `defer_javascript`: safely defer non-critical execution.
- [x] `include_js_source_maps`: preserve or emit source-map references.

### 3.5 HTML Filters
- [x] `add_head`: add a missing document head.
- [x] `combine_heads`: merge multiple head elements.
- [x] `collapse_whitespace`: remove safe excess whitespace.
- [x] `remove_comments`: strip eligible HTML comments.
- [x] `remove_quotes`: remove unnecessary attribute quotes.
- [x] `elide_attributes`: remove default-value attributes.
- [x] `convert_meta_tags`: convert eligible HTTP-equivalent meta tags to headers.
- [x] `add_instrumentation`: inject opt-in real-user measurement instrumentation.
- [x] `hint_preload_subresources`: add preload link hints.
- [x] `insert_dns_prefetch`: add DNS-prefetch hints for third parties.
- [x] `trim_urls`: shorten URLs relative to the document base.

### 3.6 Caching and URL Filters
- [x] `extend_cache`: shared content-hashed URL extension with safe long-lived browser caching.
- [x] `extend_cache_css`: CSS resource extension using the shared resource policy.
- [x] `extend_cache_scripts`: JavaScript resource extension using the shared resource policy.
- [x] `extend_cache_media`: explicitly MIME-allowlisted opaque images, PDFs, fonts, audio, video, and future binary resources.
- [x] `rewrite_domains`: resource domain mapping and rewriting.

### 3.7 Configuration and Operations Parity
- [x] Runtime on/off configuration and hierarchical inheritance.
- [x] Rewrite-level selection.
- [x] Enable, disable, and forbid individual filters.
- [x] Backend-driven file cache path, byte capacity, cleaning interval, and inode limits through `FileCacheBackend`, with deprecated path aliases.
- [x] Bounded approximate-LRU and shared-memory metadata cache; shared memory contains coordination metadata only, never cached payloads.
- [x] Memory-native RUM store with local persistence and optional Redis synchronization.
- [x] Domain mapping, sharding, proxying, and rewrite-domain configuration (`Domain`, `MapRewriteDomain`, `ShardDomain`, and `MapProxyDomain` in migration input).
- [x] Direct file loading that avoids loopback origin fetches (`LoadFromFile` in migration input).
- [x] Configurable `Vary` and forwarded-protocol handling (`RespectVary` and `RespectXForwardedProto` in migration input).
- [x] Allow/disallow URL wildcard filtering (`AllowResources` and `Disallow` in migration input).
- [x] Per-location and per-server configuration scope and inheritance.
- [x] `.laghu/console`, `.laghu/history`, and `.laghu/explain` share core models and rendering in `laghu-http`, then execute standalone, NGINX, and Apache smoke validation on parity.
- [x] Statistics, administration, message history, console, and cache-purge UI, including migration from the legacy administration/statistics/console paths.
- [x] Basic `X-Laghu` pass/bypass decision response header.
- [x] Cache-state and transform-decision response headers.
- [x] Per-request query-string filter overrides.
- [x] Purge method, cache flush file, and query-driven purge (`PurgeMethod` and purge-query migration inputs).
- [x] In-place image-resource optimization with validator-keyed cold-original and warm-variant delivery.
- [x] Client beaconing for critical-image, critical-CSS, and aggregate RUM discovery.
- [x] Experiment framework for controlled filter-set rollout.

### 3.8 Legacy Pain-Point Non-Regression
- [x] Maintain active releases, current supported NGINX compatibility, security response, and CVE handling.
- [x] Eliminate opaque binary-library dependencies and exact-version manual build breakage from supported installation paths.
- [x] Keep modern image encoders and protocol behavior maintained.
- [x] Make Core Web Vitals first-class optimization targets.
- [x] Avoid loopback image re-fetch by optimizing the response body already observed by the server adapter.
- [x] Bound and expose memory use, cache growth, and optimizer failure reasons.
- [x] Replace text-only operational surfaces with metrics, structured logs, and traces.
- [x] Keep configuration understandable through presets, validation, and explainability despite the complete filter surface.
- [x] Use Early Hints and modern resource hints instead of HTTP/2 push.

## 4. Modern Optimization Features

### 4.1 Image Pipeline
- [x] AVIF encoding and `Accept`-based negotiation.
- [ ] Flag-gated JPEG XL encoding when client support warrants it.
- [x] Modern lossy, lossless, and animated WebP encoding.
- [ ] Perceptual quality targeting with SSIMULACRA2 or DSSIM.
- [ ] Photo, screenshot, illustration, and flat-color classification with content-aware presets.
- [ ] Denoise-before-encode for suitable noisy sources.
- [ ] Mobile, tablet, and desktop viewport-width variants.
- [x] 1x and 2x pixel-density variants.
- [ ] Lower-quality `Save-Data` variants.
- [x] Client-hint-aware selection using `Sec-CH-DPR` and `Sec-CH-Viewport-Width`.
- [ ] SVG optimization and optional simple raster-to-vector conversion.
- [x] LQIP and blur-placeholder generation.
- [x] Automatic width and height injection.
- [x] LCP-image `fetchpriority=high` with safe below-fold lazy loading.
- [ ] Large animated GIF conversion to MP4/WebM video markup.

### 4.2 Core Web Vitals
- [x] Detect, preload, prioritize, and exclude the LCP element from lazy loading.
- [x] Reserve layout space for images, ads, embeds, and fonts to reduce CLS.
- [x] Add appropriate font-display behavior and font preloads.
- [x] Defer non-critical JavaScript, safely split long tasks, and delay eligible third-party scripts until interaction to improve INP.
- [x] Add HTML micro-caching, conditional revalidation (`304`), and stale-while-revalidate to improve TTFB.
- [x] Learn and apply optimization profiles by DOM-template hash.
- [x] Add an optional headless-Chrome analysis tier for critical CSS, viewport state, LCP, and rendered image dimensions.
- [x] Provide a non-blocking heuristic fallback when browser analysis is absent or fails.

### 4.3 Delivery and Protocols
- [x] Emit `103 Early Hints` for eligible preload and preconnect targets.
- [x] Inject preconnect and DNS-prefetch hints for detected third-party origins.
- [x] Validate all transforms and caching behavior under HTTP/2.
- [x] Validate all transforms and caching behavior under HTTP/3/QUIC.
- [ ] Store Brotli and Gzip pre-compressed text variants at optimization time.
- [x] Serve immutable, content-hashed resources with safe long TTLs.
- [x] Key cached variants by the bounded client-capability inputs that affect each transform.
- [ ] Emit CDN-safe headers and support origin-shield operation.

### 4.4 Safety and Security
- [x] Make transforms Content-Security-Policy-safe, including nonce and `strict-dynamic` detection and unsafe-transform auto-disable.
- [x] Respect `no-store` and `private` throughout every implemented cache and transform path.
- [x] Bypass authenticated requests in the initial eligibility policy.
- [x] Make all outbound resource fetching SSRF-safe with private and loopback access disabled by default.
- [x] Document and implement dark-mode-safe critical-CSS behavior.
- [x] Bound content size, memory, optimization time, cache growth, and variant fanout.

### 4.5 Observability and Operations
- [x] Expose Prometheus metrics for cache behavior, variants, bytes saved, per-type latency, errors, and cache size.
- [x] Emit the versioned `laghu-log-v1` schema across standalone, NGINX, Apache, and workers.
- [x] Emit OpenTelemetry traces across the optimization path.
- [x] Add unified health and readiness aggregation for the native adapters, standalone proxy, and optimizer worker.
- [x] Add an opt-in RUM beacon for LCP, INP, and CLS feedback by template.
- [x] Implement `laghu doctor` for configuration, NGINX compatibility, cache, permissions, and operational diagnostics.

### 4.6 CLI and Usability
- [x] Implement `laghu status` with authenticated ready/stats checks for standalone, NGINX, and Apache endpoints.
- [x] Implement `laghu purge <url>`.
- [x] Implement `laghu doctor`.
- [x] Implement `laghu bench`.
- [x] Implement `laghu explain <url>`.
- [x] Implement `laghu migrate` as a one-way legacy configuration converter.
- [x] Complete the documented `safe`, `balanced`, `aggressive`, `ecommerce`, `blog`, and `static` preset behavior.
- [x] Reject unsupported preset values during NGINX configuration loading.
- [x] Validate every directive and invalid cross-directive combination during configuration loading with actionable errors.
- [x] Add a web console with statistics, per-URL explanations, purge, before/after comparisons, and filter controls.
- [x] Add `?laghu=off`, `?laghu=explain`, and `?laghuFilters=...` request debugging.
- [x] Add dry-run/preview mode that reports changes without serving them.

### 4.7 Migration Converter Coverage
- [x] Convert legacy runtime on/off configuration.
- [x] Convert legacy rewrite levels to Laghu presets.
- [x] Convert legacy enabled-filter lists to Laghu filter groups.
- [x] Convert legacy cache paths to Laghu cache paths.
- [x] Convert legacy disallow rules to Laghu disallow rules.
- [x] Convert legacy administration and console endpoints.
- [x] Convert legacy query-filter overrides.
- [x] Convert legacy in-place resource optimization configuration.
- [x] Keep all legacy tokens isolated to the converter input parser and migration/background documentation.

## 5. Performance and Correctness Rail

### 5.1 Baseline Substrates
 - [x] Add a compatibility job that tries legacy module builds from newest to older candidate NGINX releases.
 - [x] Record the highest successful release as `baseline_nginx_version` after a runtime smoke test.
 - [x] Freeze and archive the successful legacy baseline Docker image.
- [ ] Provide a plain-NGINX fallback baseline with equivalent hand-written compression, cache-header, and format-selection configuration.
- [x] Build the product comparison image on a pinned current NGINX release.
- [ ] Run every benchmark against plain NGINX, the reproducible legacy baseline, and Laghu on current NGINX.

### 5.2 Deterministic Content Corpus
- [x] Generate HTML at 1 KB, 10 KB, 100 KB, 1 MB, and 5 MB.
- [ ] Cover sparse, asset-heavy, inline-CSS-heavy, inline-JS-heavy, comment-heavy, and deeply nested HTML.
- [x] Generate CSS at 1 KB, 10 KB, 100 KB, 1 MB, and 2 MB.
- [ ] Cover import-heavy, URL-heavy, unused-rule-heavy, already-minified, and framework-scale CSS.
- [x] Generate JavaScript from 1 KB through 2 MB.
- [ ] Cover already-minified, source-mapped, module, classic, render-blocking, and deferred JavaScript.
- [x] Generate JPEG, PNG, static/animated GIF, WebP, AVIF, and SVG sources.
- [x] Cover 100 px, 480 px, 768 px, 1440 px, 4K, and oversized 8000 px images.
- [ ] Cover image payloads from 5 KB thumbnails through the 10 MB processing cap.
- [ ] Cover photo, screenshot, flat illustration, noisy, transparent, logo, and icon image classes.
- [ ] Cover WebP/AVIF acceptance, `Save-Data`, 1x/2x DPR, and mobile/tablet/ desktop viewport capability permutations.
- [ ] Generate WOFF2, WOFF, subsettable, full, and icon-font cases.
- [ ] Cover empty responses, `no-store`, already-hashed URLs, huge query strings, malformed HTML, mixed content, and non-UTF-8 inputs.

### 5.3 k6 Load and Correctness Driver
- [x] Add cold-cache scenarios for first-hit optimization latency and TTFB.
- [x] Add warm-cache scenarios for steady-state delivery latency.
- [x] Sweep 1, 10, 50, 100, 500, and 1000 virtual users.
- [x] Add sustained ramp and soak scenarios for memory growth and cache thrash.
- [x] Add same-URL cache-storm scenarios that verify optimization deduplication.
- [x] Add capability variant-fanout scenarios and cache-growth checks.
- [x] Add realistic mixed page loads with HTML, images, CSS, and JavaScript.
- [x] Capture TTFB, p50/p90/p95/p99 response time, throughput, and error rate.
- [x] Capture original/optimized wire bytes and byte-savings percentage.
- [x] Capture cache hit/miss ratios and assert the expected cache state.
- [x] Capture and assert which transforms fired.
- [x] Validate parsed HTML and executable CSS/JavaScript behavior.
- [x] Validate that image variants decode and match requested formats.
- [x] Enforce the never-larger guarantee.
- [x] Assert that excluded paths and responses remain untouched.
- [x] Assert expected CLS-critical dimensions and attributes.
- [x] nginx/plain | nginx/pagespeed | nginx/laghu | standalone/laghu/plain | standalone/laghu/all-optimizations
- [x] apache/plain | apache/pagespeed | apache/laghu | standalone/laghu/plain | standalone/laghu/all-optimizations

### 5.4 Resource, Quality, and CWV Instrumentation
- [x] Capture cAdvisor/cgroup CPU, RSS, and CPU per optimized byte.
- [x] Capture optimizer latency histograms from product metrics.
- [x] Calculate SSIMULACRA2 or DSSIM for every optimized image.
- [x] Run Lighthouse/CDP measurements for LCP, INP, CLS, and TBT on representative pages.

## 6. Production Architecture
- [x] Keep policy resolution, hashing, image/CSS/HTML parsing, catalogs, queue protocol, and cache publication independent of NGINX and Apache types.
- [x] Support bounded HTTP/1.1 origin forwarding, connection and header limits, request cancellation, upstream timeouts, and original-response fallback.
- [x] Require verified TLS-to-origin and enforce an explicit trusted-forwarding policy.
- [ ] Add persistent origin connections and bounded per-origin pooling.
- [ ] Add downstream TLS termination.
- [x] Add standalone health/readiness, graceful drain, structured access and optimization logs, and rootless/read-only-container operation without making telemetry or a hosted service mandatory.
- [x] Keep the standalone proxy entirely self-hostable with no account, license key, feature gate, phone-home behavior, or required Codevedas service.
- [x] Complete the image-path two-process architecture for both lightweight NGINX and Apache interceptors plus the asynchronous out-of-process service.
- [x] Serve the original image on first hit while an optimized variant is generated without blocking NGINX event loops or Apache request workers.
- [x] Keep the memory-mapped queue and atomic disk cache interoperable across both adapters and `laghu-libvips`.
- [x] Add bounded approximate-LRU eviction.
- [x] Add per-URL purge.
- [x] Reuse `laghu-libvips`, the queue, catalogs, and cache from standalone proxy mode; `laghu-libvips` remains a transform worker and never becomes an HTTP proxy.
- [x] Make worker, queue, cache, and image-optimization failures fail open.
- [x] Generate deterministic content-hashed variant URLs for fleet-safe caching.

## 7. Open Source and Support
- [x] License the repository under MIT.
- [x] Publish a low-friction professional-support page.
- [ ] Document setup, tuning, migration, incident, and retainer support paths.

## 8. Packaging, Publication, and Release
- [ ] Ship three user-facing offerings: `ngx-laghu`, `mod-laghu`, and `laghu`; keep `laghu-libvips` an automatically installed internal dependency rather than a fourth product offering. The two native adapter packages are validated; the standalone package remains pending.
- [ ] Build and smoke the Apache 2.4 `mod_laghu` output filter on Linux and macOS x86_64/arm64, including event, worker, and prefork MPMs.
- [x] Validate Apache repeated bucket brigades, metadata buckets, `FLUSH`, `EOS`, proxied responses, HTTP/1.1, and HTTP/2 without duplicate output or lost data.
- [x] Validate Homebrew `ngx-laghu` and `mod-laghu` formula installation, loading, configuration checks, upgrades, and uninstall cleanup.
- [x] Run the local Docker matrix for Debian, Ubuntu, Fedora, Rocky, and AlmaLinux with both adapters on amd64 and arm64.
- [x] Compile the dynamic module against pinned current stable and mainline NGINX source versions in CI.
- [x] Build and install against the older NGINX 1.20 header ABI used by EL9, including pre-1.23 array-based multi-value response headers.
- [ ] Build distribution-ABI-bound adapter and service package sets for Debian and RPM systems on native Linux x86_64 and arm64 runners.
- [ ] Build and cold/warm smoke separate NGINX and Apache adapter containers on native Linux x86_64 and arm64 runners.
- [ ] Build and cold/warm smoke a standalone `laghu` proxy container on native Linux x86_64 and arm64 against NGINX, Apache, and application-server origins.
- [x] Test libraries, the POSIX runtime, `laghu-libvips`, and launchd service lifecycle natively on macOS Apple Silicon.
- [ ] Test libraries and `laghu-libvips` on Linux x86_64/arm64 and macOS Intel.
- [ ] Add tested OpenResty compatibility.
- [ ] Add tested Angie compatibility.
- [ ] Add tested freenginx compatibility.
- [x] Decide the production EL9 libvips supply chain: publish Codevedas-built `vips` and codec dependencies in the Laghu RPM repository; keep verified Remi Safe artifacts limited to development bootstrap tests.
- [ ] Build, sign, and install-test the Codevedas EL9 `vips` and codec dependency RPM set selected by the supply-chain decision.
- [x] Test installing each adapter on a clean host where NGINX or Apache is not preinstalled, proving that package dependencies select the correct server and reject an incompatible ABI.
- [ ] Add Alpine/musl packages and validation.
- [ ] Add ppc64le, s390x, riscv64, and 32-bit ARM build evidence where supported.
- [ ] Test package install, upgrade, downgrade rejection, ABI mismatch rejection, service restart, and uninstall cleanup on every supported distribution.
- [ ] Add Debian, Ubuntu, Fedora, RHEL, Rocky, and AlmaLinux release matrices rather than treating one deb and one rpm distribution as universal evidence.
- [ ] Test production containers under rootless and read-only operation, persistent-cache upgrades, rolling shutdown, and orchestrator probe behavior.
- [ ] Publish prebuilt dynamic modules for every supported NGINX release.
- [ ] Publish deb packages.
- [ ] Publish rpm packages.
- [ ] Publish production `ngx-laghu`, `mod-laghu`, and standalone `laghu` Docker images.
- [ ] Publish the standalone proxy through deb, rpm, Homebrew, GHCR, and GitHub Releases under the `laghu` package identity.
- [ ] Publish a Helm chart.
- [ ] Sign package repositories and container manifests, publish SBOMs and build provenance, scan release artifacts, and verify signatures in installation CI.
- [ ] Maintain LTS branches with a published CVE and security-response process.
- [ ] Publish a complete migration guide for legacy module and CDN-optimizer users.
