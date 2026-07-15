---
title: Security
nav_order: 7
permalink: /security/
---

# Security

Laghu treats response rewriting as a privileged parsing boundary.

## Current Defaults

- disabled unless `laghu on` is configured
- honors `rewrite_level passthrough` before entering any transformation path
- bypasses requests carrying authorization credentials
- bypasses responses marked `private` or `no-store`
- bypasses `/api` and `/graphql` path segments unless narrowly overridden
- bypasses unsupported status codes and content types
- performs no outbound resource fetches
- never loads a generic ImageMagick fallback for untrusted image bytes
- accepts image type from decoded magic and an explicit loader, not the
  declared response content type
- caps input at 10 MiB, dimensions at 8192, decoded area at 64 megapixels,
  animation at 300 frames, and isolated work at 30 seconds
- preserves animation frames, delays, loop count, and alpha; lossless output
  must decode to identical normalized pixels
- preserves the caller-owned original view when a transform candidate fails
  validation, is identical, or is not strictly smaller
- derives fleet-stable variant keys with dependency-free SHA-256 and a
  versioned canonical input
- keeps experimental filter permission disabled except for the explicit
  `experimental` rewrite level

## Worker Isolation

NGINX never invokes a codec command or waits for conversion. Each job runs in a
child process that the worker terminates at the deadline. Runtime files are
group-restricted, cache publication is atomic, and missing/corrupt output is a
cache miss. The shipped systemd unit denies network access and applies a
read-only filesystem plus private temporary state.

ImageMagick-backed generic loaders are not selected. A custom libvips build
that includes such a loader does not expand Laghu's accepted magic/loader list.

## Required Properties for Future Transforms

Every transform must be bounded by input size and time, preserve the original,
respect Content Security Policy, avoid private-network fetching by default, and
publish a variant only through the core candidate gate after complete
validation. Native codecs and parsers require dependency tracking, fuzzable
boundaries, and security review.

See the repository [security policy](https://github.com/Code-Vedas/laghu/blob/main/SECURITY.md)
for private reporting instructions.
