# 5. Laghu

## One-line positioning

**Laghu is the modern, maintained successor to the discontinued PageSpeed server modules, with equal native `ngx-laghu` and `mod_laghu` adapters that rewrite HTML, CSS, JavaScript, images, and fonts at the edge in real time.**

## Core promise

For a decade, `ngx_pagespeed` (and its Apache sibling `mod_pagespeed`) let any operator drop one module into their web server and instantly get automatic image transcoding, CSS/JS minification, critical-CSS inlining, cache extension, lazy loading, and dozens of other optimizations — with zero application changes.

Then Google **archived the project (EOL, no more releases, no security patches, no new NGINX compatibility)**. The result:

* The module no longer builds against modern NGINX releases.
* It has no AVIF, no modern Core Web Vitals awareness (LCP/INP/CLS), no HTTP/3 story.
* PSOL (the shared optimization library) is frozen and unmaintained.
* Operators running it are stuck on old NGINX and carry unpatched C++ in their edge.

Laghu answers a simple question that thousands of sysadmins, hosts, and agencies still ask every week:

> "The PageSpeed modules are dead. What do I install instead for current NGINX or Apache?"

Laghu is that answer. It does **not** just port the old modules — it implements one shared engine behind first-class NGINX and Apache adapters, then adds current formats, protocols, observability, and usability.

The name: **laghu (लघु)** is Sanskrit for *light, quick, small, nimble* — the exact property the module gives every page it touches. It fits the Codevedas naming family and reads cleanly on a CLI (`laghu status`, `laghu purge`, `laghu doctor`).

## Naming & trademark policy

To avoid any trademark challenge, the shipped product carries **zero "PageSpeed" branding**. "PageSpeed" (and `ngx_pagespeed` / `mod_pagespeed`) is **never** used in ngx-laghu's directives, response headers, CLI, config keys, endpoints, package names, or Docker images — everything is `laghu` / `X-Laghu` / `/laghu/...`.

The name is referenced in exactly **one place**: a short **"Why ngx-laghu exists?"** background section in the public documentation, purely to explain the history and help ex-users find the project. The legacy directive names surface only as *input* to the one-way `laghu migrate` converter (which reads an old config and emits a `laghu` one) and in this internal spec's audit and benchmark sections — never in the running product.

## Primary buyers

* Operators and sysadmins who ran `ngx_pagespeed` and now have nothing to replace it.
* Managed WordPress / WooCommerce / Magento hosts wanting a server-level speed layer for all tenants.
* Agencies who need Core Web Vitals wins without touching client application code.
* VPS/self-hosted owners who cannot afford a commercial CDN optimizer (Cloudflare Polish/Mirage, Fastly IO, Akamai).
* CDN and reverse-proxy operators wanting an origin-shield optimization tier.
* SaaS platforms serving many customer sites from shared NGINX.
* E-commerce stores where image weight and render-blocking assets cost conversions.
* Regulated / air-gapped environments that need on-prem optimization with no third-party pixel.

## Why this is a strong idea

* **A real, unserved gap.** The incumbent is dead. Search demand for "ngx_pagespeed alternative", "ngx_pagespeed nginx 1.25", "mod_pagespeed replacement", "compile ngx_pagespeed 2024/2025" is steady and unanswered. There is no maintained drop-in.
* **Server-level, not app-level.** It optimizes any backend — WordPress, static, Node, PHP, Rails, Django, headless — because it works on the HTTP response, not the framework.
* **Zero application changes.** The single biggest reason PageSpeed was loved: `pagespeed on;` and you are done.
* **Clear, low-maintenance monetization.** Fully open source (MIT); the only revenue path is organic professional-support leads via a "Need professional support?" page — no billing, licensing, or SaaS to operate.
* **Defensible moat.** The performance-testing rail (below) that proves each optimization across every content type and data size is a marketing and trust asset no competitor publishes.

## Business outcomes

* Restore maintained, native NGINX and Apache optimization paths for the entire ex-PageSpeed userbase.
* Improve Core Web Vitals (LCP, INP, CLS) at the server, measurably, per template.
* Cut origin egress and image bandwidth (WebP/AVIF) without app changes.
* Give hosts a per-tenant speed feature they can upsell.
* Replace fragile hand-rolled NGINX optimization snippets with one audited module.
* Provide evidence — every optimization benchmarked with k6 across sizes and content types.

---

# Part A — Full audit of PageSpeed (what the old module did)

This is the complete inventory of what `ngx_pagespeed` / `mod_pagespeed` (PSOL 1.13–1.15) shipped. ngx-laghu must match every capability here before adding anything, so nothing regresses for a migrating operator.

## A1. Rewrite levels (presets)

The old module grouped filters into levels. ngx-laghu keeps the same mental model:

* **PassThrough** — module loaded, no rewriting.
* **CoreFilters** — the safe, recommended default set.
* **OptimizeForBandwidth** — byte-savings only, no HTML structure changes (safest for fragile sites).
* **All / Experimental** — every filter including risky ones.

## A2. Image filters (complete list)

* `rewrite_images` — master image optimizer (enables sub-filters).
* `recompress_images` — lossless recompression.
* `recompress_jpeg` — JPEG-specific recompression.
* `recompress_png` — PNG-specific recompression.
* `recompress_webp` — WebP recompression.
* `convert_jpeg_to_progressive` — baseline → progressive JPEG.
* `convert_jpeg_to_webp` — JPEG → WebP for capable browsers.
* `convert_png_to_jpeg` — PNG → JPEG when no transparency.
* `convert_gif_to_png` — GIF → PNG.
* `convert_to_webp_lossless` — PNG/GIF → lossless WebP.
* `convert_to_webp_animated` — animated GIF → animated WebP.
* `jpeg_sampling` — chroma subsampling to 4:2:0.
* `resize_images` — resize to `<img>` attribute dimensions.
* `resize_rendered_image_dimensions` — resize to actual rendered size.
* `resize_mobile_images` — smaller placeholders for mobile.
* `responsive_images` — generate `srcset` for multiple resolutions.
* `responsive_images_zoom` — zoom-aware responsive variants.
* `insert_image_dimensions` — add width/height to prevent CLS.
* `inline_images` — small images as `data:` URIs.
* `inline_preview_images` — low-quality image placeholders (LQIP).
* `dedup_inlined_images` — replace repeated inlined images with a JS reference.
* `lazyload_images` — defer offscreen image loading.
* `sprite_images` — combine CSS background images into a sprite.
* `strip_image_meta_data` — remove EXIF/metadata.
* `strip_image_color_profile` — remove ICC color profiles.
* `in_place_optimize_for_browser` — browser-specific in-place optimization.

## A3. CSS filters (complete list)

* `rewrite_css` — minify CSS and rewrite embedded URLs.
* `combine_css` — combine multiple stylesheets into one.
* `inline_css` — inline small external CSS.
* `outline_css` — externalize large inline CSS blocks.
* `flatten_css_imports` — inline `@import` chains.
* `inline_import_to_link` — convert `@import` to `<link>`.
* `inline_google_font_css` — inline Google Fonts CSS.
* `move_css_to_head` — move `<link>` into `<head>`.
* `move_css_above_scripts` — move CSS above `<script>` tags.
* `prioritize_critical_css` — inline above-the-fold CSS, defer the rest.
* `rewrite_style_attributes` — apply CSS rewriting to inline `style` attributes.
* `rewrite_style_attributes_with_url` — same, for styles containing `url()`.
* `fallback_rewrite_css_urls` — rewrite URLs in unparseable CSS.

## A4. JavaScript filters (complete list)

* `rewrite_javascript` — minify JS.
* `rewrite_javascript_external` — minify external JS files.
* `rewrite_javascript_inline` — minify inline JS.
* `combine_javascript` — combine multiple JS files.
* `inline_javascript` — inline small external JS.
* `outline_javascript` — externalize large inline JS blocks.
* `defer_javascript` — defer JS execution until after load.
* `include_js_source_maps` — preserve source maps.

## A5. HTML filters (complete list)

* `add_head` — add `<head>` if missing.
* `combine_heads` — merge multiple `<head>` elements.
* `collapse_whitespace` — remove excess HTML whitespace.
* `remove_comments` — strip HTML comments.
* `remove_quotes` — remove unnecessary attribute quotes.
* `elide_attributes` — remove default-value attributes.
* `convert_meta_tags` — turn `<meta http-equiv>` into HTTP headers.
* `add_instrumentation` — inject beacon JS to measure load time (RUM).
* `hint_preload_subresources` — add `Link: rel=preload`.
* `insert_dns_prefetch` — add `<link rel=dns-prefetch>` for third parties.
* `trim_urls` — shorten URLs relative to base.
* `pedantic` — add type attributes for HTML4 validity.

## A6. Caching & URL filters (complete list)

* `extend_cache` — content-hashed URLs with 1-year browser cache.
* `extend_cache_css` / `extend_cache_scripts` / `extend_cache_images` / `extend_cache_pdfs` — per-type cache extension.
* `local_storage_cache` — cache inlined resources in `localStorage`.
* `rewrite_domains` — apply domain mappings to resources.

## A7. Core configuration surface (old module)

* `pagespeed on|off|unplugged;`
* `pagespeed RewriteLevel <level>;`
* `pagespeed EnableFilters` / `DisableFilters` / `ForbidFilters`.
* File-backed cache: `FileCachePath`, size limits, clean interval, inode limits.
* In-memory LRU cache and shared-memory metadata cache.
* Optional memcached tier.
* `Domain` / `MapRewriteDomain` / `ShardDomain` / `MapProxyDomain` (domain sharding & proxying).
* `LoadFromFile` (read origin resources from disk, skip the loopback fetch).
* `RespectVary`, `RespectXForwardedProto`.
* `AllowResources` / `Disallow` URL wildcard filtering.
* Per-`location` / per-`server` scoping and inheritance.
* Statistics + admin pages: `/pagespeed_admin`, `/ngx_pagespeed_statistics`, `/pagespeed_console`, message history, cache purge UI.
* `X-PageSpeed` response header and `?PageSpeedFilters=` query-string overrides.
* `PurgeMethod` / cache flush file / `?PageSpeed=purge`.
* IPRO ("in-place resource optimization") — optimize resources fetched without HTML rewriting.
* Beaconing / critical-image + critical-CSS detection via client beacon.
* Furious / experiment framework (A/B rollout of filter sets).

## A8. Known pain points of the old module (what to fix, not copy)

* **Dead upstream** — no releases, no CVE response, no new NGINX support.
* **Painful builds** — dynamic module compiled against exact NGINX version; PSOL binary blobs; constant breakage.
* **No AVIF**, no JPEG XL, dated WebP encoder.
* **No native Core Web Vitals model** — filters predate LCP/INP/CLS.
* **Loopback re-fetch** cost — module fetches resources back through the server.
* **Memory footprint** and opaque failure modes.
* **Weak observability** — text stats pages, no metrics endpoint, no structured logs.
* **Configuration sprawl** — dozens of camelCase directives, easy to misconfigure.
* **No HTTP/2 push replacement**, no Early Hints, no 103 story.

---

# Part B — Laghu features (parity + expansion)

## B1. Everything above, re-implemented and maintained

ngx-laghu ships **full functional parity** with the audit in Part A: every image/CSS/JS/HTML/caching optimization, the same rewrite-level presets, per-location scoping, cache extension, IPRO, admin/stats, and purge. A migrating operator maps every old directive to its `laghu` equivalent with the one-way `laghu migrate` converter (below).

## B2. Modern image pipeline (added)

* **AVIF** encode/serve with `Accept`-based negotiation.
* **JPEG XL** (behind a flag, where browser support warrants).
* Modern **WebP** encoder (lossless + lossy + animated).
* **Perceptual quality targeting** (SSIMULACRA2 / DSSIM) instead of fixed quality — hit a visual-quality score, not a magic number.
* **Content-aware encoding** — classify photo / screenshot / illustration / flat-color and pick presets.
* **Denoise-before-encode** for noisy sources (better compression).
* **Per-viewport variants** (mobile / tablet / desktop target widths).
* **Pixel-density (1x / 2x Retina) variants.**
* **`Save-Data` variants** — lower-quality alternates when the client asks to save data.
* **Client-hints aware** (`Sec-CH-DPR`, `Sec-CH-Viewport-Width`) variant selection.
* **SVG optimization** and optional raster→SVG vectorization for simple logos/icons.
* **LQIP / blur placeholder** generation for smoother loads.
* **Automatic `width`/`height` + `aspect-ratio`** injection to kill CLS.
* **`fetchpriority=high`** on the detected LCP image; lazy-load everything below the fold except LCP.
* **Animated GIF → video (`<video>` MP4/WebM)** option for large animations.

## B3. Core Web Vitals awareness (added — the big one)

The old module optimized bytes. ngx-laghu optimizes **the metrics Google actually ranks on**:

* **LCP** — detect the LCP element, preload it, set `fetchpriority`, emit `103 Early Hints`, avoid lazy-loading it.
* **CLS** — inject dimensions/aspect-ratio, reserve space for ads/embeds/fonts, add `font-display` and font preloads.
* **INP** — defer non-critical JS, split long tasks where safe, delay third-party scripts until interaction.
* **TTFB** — HTML micro-caching at the edge, conditional revalidation (`304`), stale-while-revalidate.
* **Per-template CWV profiles** — group URLs by DOM structure hash and apply one learned optimization profile per template.
* Optional **headless-Chrome analysis tier** to compute real critical CSS (CSS Coverage API), true above-the-fold, and real LCP/rendered-image dimensions — with a heuristic fallback so the module never blocks on Chrome.

## B4. Modern delivery & protocol support (added)

* **`103 Early Hints`** emission for preload/preconnect (replaces dead HTTP/2 push).
* **Preconnect / dns-prefetch** injection for detected third-party origins.
* **HTTP/2 and HTTP/3 (QUIC)** aware — no assumptions that break under multiplexing.
* **Brotli + Gzip pre-compressed text variants** stored at optimize time (zero CPU on cache hit).
* **`immutable` + content-hash** cache-busting with safe long TTLs.
* **`Vary` / capability-mask cache keying** so every client gets the right variant.
* **CDN-friendly** headers and an origin-shield mode.

## B5. Safety, correctness & security (added)

* **Content-Security-Policy-safe transforms** — no inline `onload`, external hashed loader scripts, nonce/`strict-dynamic` detection with auto-disable of unsafe transforms.
* **Never-larger guarantee** — a variant is only served if smaller than the original.
* **Idempotent & reversible** — every transform can be turned off per location/URL; original always recoverable.
* **`no-store` / `private` respected**; auth and API paths excluded by default.
* **SSRF-safe fetching** — private/loopback URL capture off by default.
* **Dark-mode-safe critical CSS** — documented override path so inlined critical CSS does not flash light theme.
* **Deterministic output** — same input → same hashed URL, cache-safe across a fleet.
* **Fail-open** — any optimizer error falls back to serving the original untouched.

## B6. Observability & operations (added)

* **Prometheus `/metrics`** endpoint (hit/miss, variants written, bytes saved, per-type latency, errors, cache size).
* **Structured JSON logs** with per-request optimization decisions.
* **OpenTelemetry traces** for the optimize path.
* **Health + readiness endpoints** for k8s / load balancers.
* **Real-User-Monitoring (RUM) beacon** (opt-in) feeding back real LCP/INP/CLS per template to tune profiles.
* **Grafana dashboard** shipped in-repo.
* **`laghu doctor`** — one command that inspects the running config, NGINX version, cache health, permissions, and reports misconfigurations.

## B7. Usability (added — make it easy where the old one was hard)

* **`laghu` CLI**: `laghu status`, `laghu purge <url>`, `laghu doctor`, `laghu bench`, `laghu explain <url>` (shows exactly which transforms fired and why), `laghu migrate` (convert an old `pagespeed` config).
* **Sane presets**: `laghu preset safe|balanced|aggressive|ecommerce|blog|static` instead of hand-picking 60 filters.
* **One-way migration converter** (`laghu migrate`): reads an existing legacy config file and emits an equivalent `laghu` config. ngx-laghu's own runtime never defines a `pagespeed` directive — the legacy names live only in the converter's input parser.
* **Config validation on load** — refuse to start with a clear error instead of silent misbehavior.
* **Web console** — live stats, per-URL "explain", one-click purge, before/after byte and CWV comparison, filter toggles.
* **Query-string debug** — `?laghu=off`, `?laghu=explain`, `?laghuFilters=...` for quick per-request testing.
* **Dry-run / preview mode** — see what *would* change without serving it.

## B8. Packaging & support (added — the reason it survives where the old one died)

* **Prebuilt server adapters** for every supported NGINX and Apache release/ABI, with a build matrix in CI.
* **Distro packages** (deb/rpm), **Docker images**, and a **Helm chart**.
* **Apache 2.4**, **OpenResty / Angie / FreeNginx** compatibility targets.
* **A sidecar / reverse-proxy mode** for environments that cannot load a custom server module (runs as a standalone optimizing proxy in front of any origin).
* **LTS branches** with a published security-response policy and CVE process — the thing the archived project fatally lacked.
* **Migration guide** from `ngx_pagespeed` and from Cloudflare Polish/Mirage.

## B9. Legacy directive → ngx-laghu mapping (one-way `laghu migrate` converter)

This table is the converter's translation map, not a set of runtime-accepted directives. The legacy tokens exist only in the converter's input parser; the running module only ever understands `laghu` directives.

| Legacy config input | ngx-laghu output |
| --- | --- |
| `pagespeed on;` | `laghu on;` |
| `pagespeed RewriteLevel CoreFilters;` | `laghu preset balanced;` |
| `pagespeed EnableFilters rewrite_images,...;` | `laghu enable image,css,js,...;` |
| `pagespeed FileCachePath /var/cache;` | `laghu cache_path /var/cache/laghu;` |
| `pagespeed Disallow "*/admin/*";` | `laghu disallow /admin/;` |
| `/pagespeed_admin`, `/pagespeed_console` | `/laghu/console`, `/metrics` |
| `?PageSpeedFilters=` | `?laghuFilters=` |
| IPRO | on by default, `laghu ipro on;` |

---

# Part C — Performance testing rail (paramount, and the hard part)

> The problem you flagged: there is **no active PageSpeed** we can just install and benchmark against on current systems. So the testing strategy cannot assume a live upstream module. It must (1) discover a working substrate, (2) build both the baseline and ngx-laghu, and (3) exercise **every optimization, on every content type, at every data size**, with k6 as the load driver — producing repeatable, published numbers.

## C1. Why this rail matters

* It is the **trust asset**: migrating operators must see "ngx-laghu is at least as fast and lighter than what you had, and here is the proof at every size."
* It is the **regression net**: no optimization ships without a benchmark that proves it helps (or at least never hurts) across sizes.
* It is **marketing**: publishable "bytes saved / LCP delta / CPU cost" curves per content type.

## C2. Rail 1 — establishing a runnable baseline substrate

Because the upstream module is discontinued, the harness first **finds and pins the newest environment that can still run the legacy `ngx_pagespeed`**, purely as a comparison baseline:

* A **compatibility matrix job** iterates candidate NGINX releases (newest → older) and attempts to build the archived `ngx_pagespeed` + PSOL against each.
* The **highest NGINX version that compiles and passes a smoke test** is recorded as `baseline_nginx_version` (the "last supported release").
* That exact build is **frozen into a Docker image** (`laghu-baseline:nginx-<ver>`), so the historical baseline is reproducible forever even as the upstream rots.
* If no legacy build succeeds on any recent NGINX, the baseline falls back to **plain NGINX with equivalent hand-rolled directives** (gzip/brotli, cache headers, `image/webp` via `try_files`) — a "what people do without PageSpeed" baseline.
* ngx-laghu is built on **current NGINX** in a parallel image (`laghu:nginx-<current>`).

This yields three comparison targets:

1. **Plain NGINX** (no optimization) — the floor.
2. **Legacy ngx_pagespeed** on its last-supported NGINX — the incumbent.
3. **ngx-laghu** on current NGINX — the product.

## C3. Rail 2 — the content corpus (every content type × every data size)

A generated, deterministic corpus so results are comparable run-to-run. Each dimension is swept across sizes.

**HTML documents:**
* Tiny (1 KB), small (10 KB), medium (100 KB), large (1 MB), huge (5 MB).
* Variants: few assets vs asset-heavy (100+ subresources), inline-CSS-heavy, inline-JS-heavy, comment-heavy, deeply nested DOM.

**CSS:**
* 1 KB, 10 KB, 100 KB, 1 MB, 2 MB (near the processing cap).
* Variants: many `@import`s, lots of `url()`, unused-rule-heavy, minified-already, framework bundles (Bootstrap/Tailwind-scale).

**JavaScript:**
* 1 KB → 2 MB.
* Variants: already-minified, source-mapped, module vs classic, render-blocking vs deferred.

**Images (the heavy axis):**
* JPEG, PNG, GIF (static + animated), WebP, AVIF, SVG sources.
* Dimensions: 100px, 480px, 768px, 1440px, 4K, and oversized (8000px).
* Byte sizes: 5 KB thumbnail → 10 MB near the processing cap.
* Content classes: photo, screenshot, flat illustration, noisy, transparent PNG, logo/icon.
* Client capability permutations: `Accept: image/webp`, `image/avif`, `Save-Data: on`, 1x vs 2x DPR, mobile/tablet/desktop viewport hints.

**Fonts:** WOFF2/WOFF, subsettable vs full, icon fonts.

**Edge cases:** empty responses, `no-store`, already-hashed URLs, huge query strings, malformed HTML, mixed content, non-UTF-8.

## C4. Rail 3 — k6 as the load and correctness driver

k6 scripts drive load and assert both **performance** and **correctness** for every corpus item against all three targets.

**k6 scenarios (per optimization, per content type, per size):**

* **Cold cache** — first request (cache MISS): measures worst-case optimize latency / TTFB.
* **Warm cache** — repeated request (cache HIT): measures steady-state serve latency.
* **Concurrency sweep** — 1, 10, 50, 100, 500, 1000 VUs (virtual users) to find the throughput knee and CPU/memory ceiling.
* **Ramp / soak** — sustained load to catch memory growth and cache thrash.
* **Cache-storm** — N simultaneous first-hits on the same URL to verify notification dedup (only one optimize pass fires).
* **Variant-fanout** — same URL requested with every capability permutation to measure variant explosion cost and cache size.
* **Mixed-realistic** — a weighted blend modeling a real page load (1 HTML + 30 images + 5 CSS + 10 JS).

**k6 metrics captured per run:**

* TTFB, full response time (p50/p90/p95/p99), throughput (req/s), error rate.
* Bytes on the wire (original vs optimized) → **byte-savings %**.
* Cache HIT/MISS ratio (from the `X-Laghu` header — or the legacy target's own cache header when benchmarking the baseline — via a k6 `Check`).
* Which transforms fired (via `laghu explain` header) — asserted so a filter that should trigger did.

**k6 checks (correctness gates — a fast wrong answer is a failure):**

* Response is byte-identical-or-smaller and still valid (HTML parses, CSS/JS still execute).
* Image variant is decodable and matches the requested `Accept` format.
* Never-larger guarantee holds (optimized ≤ original).
* No transform fired on excluded paths (`/admin`, `/api`, `no-store`).
* CLS-critical attributes (`width`/`height`) present when expected.

## C5. Rail 4 — resource + quality instrumentation (beyond k6)

k6 measures the client side; the harness also captures the server side and visual quality:

* **cAdvisor / cgroup stats** per container: CPU seconds, RSS, and CPU-per-optimized-byte for each optimization.
* **Optimize latency histograms** from the module's own `/metrics`.
* **Visual quality**: SSIMULACRA2 / DSSIM of every optimized image vs original, so "bytes saved" is never reported without "quality retained".
* **CWV lab measurement**: a headless-Chrome pass (Lighthouse/CDP) on representative pages through each target, capturing LCP/INP/CLS/TBT — so the story is "faster metrics", not just "smaller bytes".

## C6. Rail 5 — the result matrix (what gets published)

For **every (optimization × content type × size × scenario)** cell, the rail emits:

| Dimension | Reported |
| --- | --- |
| Target | plain / legacy pagespeed / ngx-laghu |
| Content type & size | e.g. JPEG photo 2 MB @ 1440px |
| Scenario | cold / warm / concurrency N |
| Bytes | original → optimized (% saved) |
| Latency | TTFB & p95 response time |
| Throughput | req/s at the knee |
| Server cost | CPU-ms & RSS per request |
| Quality | SSIMULACRA2 delta |
| CWV | LCP / INP / CLS delta |
| Verdict | faster / neutral / regression (gate) |

Output as machine-readable JSON + a rendered HTML report and Grafana snapshots. A **regression gate in CI** fails the build if any cell shows ngx-laghu slower or heavier than plain NGINX beyond a tolerance, or worse than the legacy baseline on a core filter.

## C7. Rail 6 — reproducibility & automation

* Entire rail is **containerized and one-command** (`laghu bench --full` / `make bench`).
* Deterministic corpus generator (seeded) so runs are comparable across machines and over time.
* Runs in **CI on every release** and on a schedule; historical results tracked to show trend lines (no perf drift release-to-release).
* Baseline images are **pinned and archived**, so the "last NGINX that ran ngx_pagespeed" comparison stays reproducible even after the upstream is fully unbuildable.
* Hardware profiles recorded (CPU model, cores) so numbers are interpreted in context.

---

# Part D — Architecture notes

* **Two-process model**: lightweight NGINX and Apache adapters serve from a shared cache, while the out-of-process **`laghu-libvips` service** performs heavy CPU work asynchronously — so slow optimization never blocks the response. First hit serves the original; the optimized variant lands in cache for subsequent hits.
* **Shared memory-mapped cache** readable by both NGINX and the worker; LRU eviction; per-URL purge.
* **Sidecar mode** for no-custom-module environments: the same worker runs as a standalone reverse proxy.
* **Fail-open everywhere**: worker down, socket missing, optimize error → original content is served unchanged.
* **Deterministic, content-hashed variant URLs** for safe long-TTL caching across a fleet.

---

# Part E — Licensing & monetization

**Laghu is fully open source (MIT), like the rest of the Codevedas family (e.g. [Kaal](https://kaal.codevedas.com)).** There is no open-core, no paid tier, no SaaS, no license key, and no feature gating. Every optimization, the CLI, the benchmark rail, and all packages ship free.

The **only** monetization is **organic professional-support leads** — the same model as `kaal.codevedas.com`:

* Docs and the project site carry a single, low-friction **"Need professional support?"** call to action (`laghu.codevedas.com/support`).
* It converts a slice of the free userbase — operators who hit a tuning wall, a tricky migration, a CWV target, or a production incident — into paid engagements (setup, tuning, migration, incident help, retainers).
* No dark patterns, no nag screens, no crippled free version: the product is complete on its own, and support is offered, never forced.
* Leads compound across the portfolio, since the same audience is served by the other Codevedas ideas (e.g. SiteVitals Intelligence, idea 4).

---

# Part F — Positioning summary

> The PageSpeed modules are dead. Laghu is the maintained successor: one shared engine behind first-class `ngx-laghu` and `mod_laghu` adapters, with every optimization proven across both server families.
