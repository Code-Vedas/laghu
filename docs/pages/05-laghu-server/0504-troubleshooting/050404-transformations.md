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

Use `laghu explain` for cold/warm state, parser and CSP eligibility, source integrity, dependency graph, RUM readiness, candidate size, cache selection, and response-header changes.
Use dry-run mode to compare planned changes without serving them.
Resource URLs remain unchanged when a `<base href>` is ambiguous, the URL is cross-origin or already shortest, the attribute is not resource-fetching, or the complete `srcset` cannot be validated.

If a transformation breaks behavior, disable the narrow filter or route through the experiment framework, preserve the origin and derived fixtures, and retain the transaction trace.
Do not weaken CSP, remove integrity, or bypass TLS verification as a diagnostic shortcut.
