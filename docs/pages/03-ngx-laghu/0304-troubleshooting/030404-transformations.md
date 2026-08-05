---
title: Transformations and CSP
parent: Troubleshooting
grand_parent: ngx-laghu
nav_order: 4
permalink: /ngx-laghu/troubleshooting/transformations/
---

# ngx-laghu Transformations and CSP

Use `X-Laghu`, response cache/CSP headers, and authenticated metrics to identify eligibility, bounded failure classes, and selected variants.

If CSS, JavaScript, or HTML remains unchanged, check cold versus warm state, source validators, parser support, integrity/nonce attributes, relative imports, source directives, script kind, later cascade boundaries, and aggregate transfer size.
Resource URLs remain unchanged when a `<base href>` is ambiguous, the URL is cross-origin or already shortest, the attribute is not resource-fetching, or the complete `srcset` cannot be validated.
If an image format is absent, check `Accept`, client hints, source magic, dimensions, codec capability, animation, quality policy, and whether the candidate was actually smaller.

Laghu intersects every enforcing CSP response header and bounded CSP meta policy. Report-only headers do not restrict transforms. It honors element/attribute directive precedence, `'self'`, source schemes and origins, `data:`, nonces, hashes, `'unsafe-inline'`, `'unsafe-hashes'`, and `'strict-dynamic'`. Existing valid nonces are preserved only on the transformed element; integrity-bearing and hash-only inline content remains byte-identical. Under nonce/hash-based `'strict-dynamic'`, a newly injected parser script is rejected even when `'self'` is present.

For CSP failures, preserve the application policy and configure an accepted nonce or exact same-origin source rather than adding broad unsafe directives. Laghu never rewrites the CSP header or generates a nonce. Malformed, oversized, excessive, or ambiguous policy input skips only the affected transformation and leaves the response deliverable.
Immutable `/.laghu/` routes must remain same-origin, content-addressed, publicly cacheable, and backed by a ready catalog entry.

When output is semantically wrong, disable Laghu for the narrow affected route, preserve the original and transformed bytes plus headers, and file a reproducible report.
