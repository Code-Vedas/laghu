---
title: CSS Optimization
nav_order: 5
permalink: /css-optimization/
---

# CSS Optimization

Laghu safely minifies eligible external stylesheets and HTML `style` attributes through one dependency-free tokenizer shared by NGINX and Apache. The first stylesheet response is always byte-identical to the origin. Laghu publishes a derived stylesheet in the content-addressed cache and may serve it on a later request.

The parser is limited to 2 MiB, 262,144 tokens, 256 unique image URLs, 64 nesting levels, and 32 sprite inputs. It preserves strings, escaped content, custom-property values, `calc()` spacing, source order, `/*! ... */` comments, and source-map/sourceURL comments. Ordinary comments and grammar-insignificant space are removed. Numbers, colors, selectors, declaration order, and custom property values are not normalized.

Relative and root-relative `url(...)` references are normalized against the stylesheet URL. Only same-origin HTTP(S) image dependencies are eligible. Fragments, data and blob URLs, protocol-relative and cross-origin references, and API or GraphQL paths remain unchanged. URL-looking text in strings and comments is never interpreted. If the complete parse fails, the fallback may rewrite only independently valid URL tokens; an unterminated string, comment, escape, or URL preserves every byte.

Image URLs change only after a ready catalog variant matches the original format. Unresolved dependencies preserve the entire original response, while a terminally excluded dependency retains its origin URL. Source, policy, capability, parser, and selected-variant changes invalidate the derived CSS. Rewritten responses remove stale length and digest headers and use a strong dependency-derived ETag.

Sprite generation is deliberately conservative. A stylesheet may produce one horizontal, lossless PNG sprite from two to 32 static raster images referenced by standalone `background-image` declarations with explicit `no-repeat`. Layered or shorthand backgrounds, existing positions, background sizing, animation, SVG, data URLs, and cross-origin images are excluded. Inputs are sorted by normalized URL, and the out-of-process `laghu-libvips` service reads only validated cached variants. The sprite and rewritten CSS must pass their strict size gates before publication.

Laghu may combine two to 32 strictly adjacent external stylesheet links when CSS rewriting and structural rewriting are selected. Only whitespace may separate links; comments and other nodes terminate a group. Every member must be an ordinary same-origin stylesheet with the same effective media value, ready catalog data, and ready nested image dependencies. DOM order and ordered duplicates are preserved with a newline between payloads.

Combination respects `style-src-elem`, then `style-src`, then `default-src` and requires the internal same-origin asset route to be allowed. Integrity, nonce, alternate, disabled, cross-origin and unsupported attributes exclude links. Imports, namespaces, charsets, external fonts, source maps/source URLs, malformed CSS, and unresolved relative URLs exclude the complete group. The bundle is reparsed, capped at 2 MiB, atomically published, and exposed through `/.laghu/css/<sha256>`. The first eligible page stays original; a later request may receive the combined link only when total HTML and unique CSS transfer bytes are strictly smaller. Laghu does not count request overhead as savings.

Laghu also resolves conservative leading `@import` graphs from the stylesheet catalog. It accepts quoted and `url(...)` same-origin imports with no media, `all`, or an ordinary media query. Imports using `layer()` or `supports()`, late imports, fragments, API or GraphQL paths, cross-origin URLs, charsets, namespaces, external fonts, and source-map/sourceURL comments reject the whole flattening attempt. No origin request is made.

Every imported stylesheet and nested dependency must already be ready and unexpired. The resolver detects cycles before expansion and limits a graph to 32 unique stylesheets, eight levels, and 2 MiB of finalized CSS. Relative `url(...)` values are rebased against the stylesheet that contains them, media imports are wrapped in `@media`, and source order is preserved. The first root response publishes the dependency-derived asset but stays byte-identical; a later response may use it only when the flattened transfer is strictly smaller. Changes to any source, media query, derived dependency, policy, or capability produce a new key.

For complete inline `<style>` blocks, Laghu may instead convert contiguous leading imports to ordered immutable links under `/.laghu/css/<sha256>`. The block must have absent or `all` media, valid CSS, no `scoped` attribute, and a CSP that permits same-origin stylesheet links. Non-`all` import media is kept on its generated link. An import-only block is removed; otherwise supported attributes and remaining CSS stay in place. This runs before outlining and combining, so compatible generated links can participate in combination. Unresolved or invalid dependencies preserve the entire page, and the normal cold-original lifecycle and total-transfer gate still apply.

## Head normalization and CSS placement

Before the catalog-driven inline, outline, import, and combination passes, Laghu runs one bounded document-structure planner. It adds a missing head only to a clearly complete document, and merges multiple heads only when they are adjacent with whitespace or comments between them. Fragments and malformed or ambiguous structures remain byte-identical. Intervening comments, whitespace, child order, tag attributes, and untouched source bytes are preserved.

Ordinary enabled stylesheet links and complete non-scoped style blocks may be moved into the normalized head in their original CSS order. Alternate or disabled links, event-handler attributes, malformed nodes, and CSS inside `template`, `noscript`, SVG, or MathML are excluded. Media, type, nonce, integrity, crossorigin, referrer policy, quoting, and other supported attributes are copied byte-for-byte.

Policies without script-reordering permission move only CSS that already precedes every executable classic or module script. Aggressive, blog, static, `all`, and `experimental` policies may move eligible CSS before the first executable head script. JSON, data, import-map, and speculation-rule scripts do not block placement. No script itself is modified or reordered.

Dependency-free normalization follows the normal cold-original lifecycle: the first eligible response publishes the derivation, and a later response may use it with a dependency-derived strong ETag. Byte-neutral placement is accepted; growth is accepted only when verified resource savings cover it. Encoded, partial, authenticated, private, API, oversized, or failed responses retain their original bytes.

## Critical CSS prioritization

Policies selecting critical CSS may learn viewport-critical rules through the opt-in critical-CSS beacon. Laghu groups observations by a bounded structural template fingerprint, stylesheet dependency, policy, and mobile or desktop viewport bucket. The browser reports opaque rule indexes only; page text, selectors, URLs, cookies, addresses, and client identifiers are neither reported nor stored. Three valid observations are required, observations can add but never remove rules, and metadata expires with the configured metadata TTL.

The transformation is deliberately cascade-safe. It applies only to one final ordinary same-origin stylesheet with ready, no-larger derived CSS, no other author style block, supported attributes, and CSP permission for inline and same-origin styles. Laghu inlines learned rules and conservative foundational dependencies at the original link position, then places the complete stylesheet before `</body>`. Loading the complete stylesheet restores the original rule order and does not depend on JavaScript. The critical payload is capped by `css_inline_limit`; this temporary duplication is the sole critical-CSS exception to the combined-transfer gate.
