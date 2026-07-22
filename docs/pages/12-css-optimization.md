---
title: CSS Optimization
nav_order: 5
permalink: /css-optimization/
---

# CSS Optimization

Laghu safely minifies eligible external stylesheets and HTML `style`
attributes through one dependency-free tokenizer shared by NGINX and Apache.
The first stylesheet response is always byte-identical to the origin. Laghu
publishes a derived stylesheet in the content-addressed cache and may serve it
on a later request.

The parser is limited to 2 MiB, 262,144 tokens, 256 unique image URLs, 64
nesting levels, and 32 sprite inputs. It preserves strings, escaped content,
custom-property values, `calc()` spacing, source order, `/*! ... */` comments,
and source-map/sourceURL comments. Ordinary comments and grammar-insignificant
space are removed. Numbers, colors, selectors, declaration order, and custom
property values are not normalized.

Relative and root-relative `url(...)` references are normalized against the
stylesheet URL. Only same-origin HTTP(S) image dependencies are eligible.
Fragments, data and blob URLs, protocol-relative and cross-origin references,
and API or GraphQL paths remain unchanged. URL-looking text in strings and
comments is never interpreted. If the complete parse fails, the fallback may
rewrite only independently valid URL tokens; an unterminated string, comment,
escape, or URL preserves every byte.

Image URLs change only after a ready catalog variant matches the original
format. Pending dependencies preserve the entire original response, while a
terminally excluded dependency retains its origin URL. Source, policy,
capability, parser, and selected-variant changes invalidate the derived CSS.
Rewritten responses remove stale length and digest headers and use a strong
dependency-derived ETag.

Sprite generation is deliberately conservative. A stylesheet may produce one
horizontal, lossless PNG sprite from two to 32 static raster images referenced
by standalone `background-image` declarations with explicit `no-repeat`.
Layered or shorthand backgrounds, existing positions, background sizing,
animation, SVG, data URLs, and cross-origin images are excluded. Inputs are
sorted by normalized URL, and the out-of-process `laghu-libvips` service reads
only validated cached variants. The sprite and rewritten CSS must pass their
strict size gates before publication.

Laghu may combine two to 32 strictly adjacent external stylesheet links when
CSS rewriting and structural rewriting are selected. Only whitespace may
separate links; comments and other nodes terminate a group. Every member must
be an ordinary same-origin stylesheet with the same effective media value,
ready catalog data, and ready nested image dependencies. DOM order and ordered
duplicates are preserved with a newline between payloads.

Combination respects `style-src-elem`, then `style-src`, then `default-src` and
requires the internal same-origin asset route to be allowed. Integrity, nonce,
alternate, disabled, cross-origin and unsupported attributes exclude links.
Imports, namespaces, charsets, external fonts, source maps/source URLs,
malformed CSS, and unresolved relative URLs exclude the complete group. The
bundle is reparsed, capped at 2 MiB, atomically published, and exposed through
`/.laghu/css/<sha256>`. The first eligible page stays original; a later request
may receive the combined link only when total HTML and unique CSS transfer
bytes are strictly smaller. Laghu does not count request overhead as savings.

Flattening imports, moving CSS, general `<style>` rewriting, and critical-CSS
extraction remain pending.
