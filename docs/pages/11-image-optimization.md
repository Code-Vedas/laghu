---
title: Image Optimization
nav_order: 4
permalink: /image-optimization/
---

# Image Optimization

Laghu performs image codec work only in `laghu-libvips`. The NGINX and Apache adapters are
linked to the shared policy and queue libraries, not libvips, and never
executes `vips`, `magick`, or another codec command.

## Delivery Lifecycle

1. On a cold eligible image response, either adapter streams the original to the client
   and attempts a non-blocking queue publication.
2. The worker detects JPEG, PNG, GIF, or WebP from magic, selects the matching
   explicit loader, applies the resolved plan, and validates each candidate.
3. A valid, non-identical, strictly smaller candidate is fsynced and renamed
   into the content-addressed cache.
4. A later request with the same strong origin validator, policy, and browser
   format capability can receive the published variant as `X-Laghu: image-hit`.

Without a strong validator, Laghu buffers only within the encoded-input bound,
hashes the source, and never guesses that an older URL variant still matches.
HTML discovery never fetches a resource: normal image requests supply its bytes
and publish the matching catalog variants asynchronously.

## Formats and Safety

The worker supports lossless PNG/WebP recompression, policy-gated JPEG
re-encoding, progressive 4:2:0 JPEG, capable-client JPEG-to-WebP, opaque
PNG-to-JPEG, static GIF-to-PNG, PNG/GIF-to-lossless-WebP, and animated
GIF-to-WebP. JPEG decode/re-encode is lossy and therefore never runs under the
safe preset. A transform is automatically removed when one of its explicit
load/save operations is absent. Startup probes decode immutable fixtures with
each explicit loader independently of its saver. A partial custom backend emits
one warning naming missing operations; worker job failures and no-candidate
outcomes are diagnosed at most once per minute.

Animation output must retain frame count, delay array, loop count, dimensions,
and alpha. Lossless candidates must also decode to the same normalized pixels.
Lossy output requires preset or rewrite-level permission. EXIF and eligible ICC
profiles are removed only when selected by policy.

The shared image library provides a bounded, case-insensitive HTML discovery
tokenizer, same-origin URL normalization, no-upscale geometry planning, exact
1x/2x width variants, dimension injection, native lazy loading, CSP-gated data
URIs and 24-pixel previews, and repeated-inline deduplication. Responsive
markup uses width descriptors and deterministic `sizes`; browser DPR and zoom
selection requires no injected JavaScript.

Catalog metadata is versioned and checksummed beneath the image cache. It is
keyed by normalized URL, source hash, resolved policy, and backend capability
mask, records natural dimensions even when no smaller candidate exists, caps a
source at eight widths, and expires after seven days by default. Queue protocol
v4 carries at most two geometry targets in one bounded source payload. Catalog
corruption and expiry are fail-open.

Eligible HTML is delivered unchanged during cold discovery and while any
dependency is pending. Once every referenced image is ready or terminally
excluded, both adapters can atomically inject dimensions, exact 1x/2x `srcset`
and `sizes`, native lazy loading, CSP-permitted small final images, and 24-pixel
previews. Repeated final-image inlining occurs once per unique variant. The
rewrite is rejected unless its byte growth is smaller than the minimum unique
image savings at both 1x and 2x selection, and its ETag includes every source
and selected variant dependency.

Rendered and mobile dimensions can be learned through the opt-in fixed
same-origin beacon. Its 16 KiB JSON requests contain only normalized image URL,
dimensions, viewport width, DPR, and above-fold state; Laghu stores no cookie,
IP address, client identifier, or page body. `Sec-CH-Viewport-Width` and `DPR`
provide beacon-free mobile sizing inputs when present.

Ready bytes are addressable on both adapters at
`/.laghu/image/<64-lowercase-hex-variant-key>`. The route accepts only a hash,
performs no caller-controlled filesystem lookup, revalidates the cached payload
hash before sending it, and returns `Cache-Control: public,
max-age=31536000, immutable`.

CSS sprites remain pending until the Section 3.3 CSS parser is available.

## Bounds and Failure Behavior

Defaults are 10 MiB encoded input, 8192 pixels per dimension, 64 megapixels
across decoded frames, 300 animation frames, and a 30-second isolated child
deadline. The queue has four fixed 10 MiB slots by default. A full queue,
worker crash, timeout, decode failure, oversized input, unsupported operation,
cache corruption, or non-smaller candidate preserves the original response.

Variant key version 4 includes the original hash, resolved policy and quality,
worker build/libvips identity, frozen encoder options, capability mask,
transform flags, dimensions, lossy permission, and browser WebP acceptance.
Cache metadata includes a separate hash of the published bytes, and the served
strong ETag is derived from that payload hash. Warm markup uses a dependency
hash over every ready resource source-content hash and selected variant, so an
image or policy change invalidates the page.
