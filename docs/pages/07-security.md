---
title: Security
nav_order: 7
permalink: /security/
---

# Security

Laghu treats response rewriting as a privileged parsing boundary.

## Current Defaults

- disabled unless `laghu on` is configured
- bypasses requests carrying authorization credentials
- bypasses responses marked `private` or `no-store`
- bypasses unsupported status codes and content types
- performs no outbound resource fetches
- passes the original response through without mutation

## Required Properties for Future Transforms

Every transform must be bounded by input size and time, preserve the original,
respect Content Security Policy, avoid private-network fetching by default, and
publish a variant only after complete validation. Native codecs and parsers
require dependency tracking, fuzzable boundaries, and security review.

See the repository [security policy](https://github.com/Code-Vedas/laghu/blob/main/SECURITY.md)
for private reporting instructions.

