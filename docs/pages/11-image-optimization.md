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

1. On a cold eligible image response, NGINX streams the original to the client
   and attempts a non-blocking queue publication.
2. The worker detects JPEG, PNG, GIF, or WebP from magic, selects the matching
   explicit loader, applies the resolved plan, and validates each candidate.
3. A valid, non-identical, strictly smaller candidate is fsynced and renamed
   into the content-addressed cache.
4. A later request with the same strong origin validator, policy, and browser
   format capability can receive the published variant as `X-Laghu: image-hit`.

Without a strong `ETag`, the current
NGINX path still queues the cold body but does not substitute a cache entry in
the header filter. This conservative behavior avoids associating stale bytes
with a URL. Content-hash-only buffered warm substitution is not yet exposed.

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

The shared image library also provides ready-catalog markup operations for
dimensions, responsive 1x/2x sources, zoom flags, native lazy loading, small
data URIs, low-quality previews, inline deduplication, and CSS sprite
coordinates. Those primitives do not imply that general HTML or CSS rewriting
is enabled in the NGINX delivery path.

## Bounds and Failure Behavior

Defaults are 10 MiB encoded input, 8192 pixels per dimension, 64 megapixels
across decoded frames, 300 animation frames, and a 30-second isolated child
deadline. The queue has four fixed 10 MiB slots by default. A full queue,
worker crash, timeout, decode failure, oversized input, unsupported operation,
cache corruption, or non-smaller candidate preserves the original response.

Variant key version 3 includes the original hash, resolved policy and quality,
worker build/libvips identity, frozen encoder options, capability mask,
transform flags, dimensions, lossy permission, and browser WebP acceptance.
Cache metadata includes a separate hash of the published bytes, and the served
strong ETag is derived from that payload hash. Markup helpers separately expose
a dependency hash over every ready resource source-content hash, so a changed
image invalidates the derived markup key used by a future delivery integration.
