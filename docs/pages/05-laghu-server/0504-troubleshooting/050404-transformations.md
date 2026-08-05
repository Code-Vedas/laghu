---
title: HTTP, Transformations, and CSP
parent: Troubleshooting
grand_parent: Laghu Server
nav_order: 4
permalink: /laghu-server/troubleshooting/transformations/
---

# Laghu Server HTTP, Transformations, and CSP

Separate transport failures from optimization decisions.
Confirm downstream HTTP negotiation, forwarded authority/scheme, origin framing, decompression, `Vary`, cache controls, validators, and content type before investigating a filter.

Use `X-Laghu`, cache/CSP response headers, and authenticated metrics for cold/warm state and failure classification.
Use dry-run mode to compare planned changes without serving them.
Resource URLs remain unchanged when a `<base href>` is ambiguous, the URL is cross-origin or already shortest, the attribute is not resource-fetching, or the complete `srcset` cannot be validated.

The shared policy engine intersects all enforcing CSP headers and bounded CSP meta policies; report-only headers do not restrict transforms. It applies element/attribute directive precedence and evaluates `data:`, same-origin sources, nonces, hashes, `'unsafe-inline'`, `'unsafe-hashes'`, and `'strict-dynamic'`. Existing valid nonces stay only on the transformed element. Hash-only inline content, integrity-bearing elements, and unauthorized `'strict-dynamic'` injections remain unchanged. Malformed, excessive, or uncertain policies skip the affected transform without failing the proxy response.

If a transformation breaks behavior, disable the narrow filter for the affected route and preserve the origin and derived fixtures plus response headers.
Do not weaken or rewrite CSP, generate nonces, remove integrity, or bypass TLS verification as a diagnostic shortcut.
