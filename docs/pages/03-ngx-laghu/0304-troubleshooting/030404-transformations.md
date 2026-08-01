---
title: Transformations and CSP
parent: Troubleshooting
grand_parent: ngx-laghu
nav_order: 4
permalink: /ngx-laghu/troubleshooting/transformations/
---

# ngx-laghu Transformations and CSP

Use `laghu explain <url>` to identify eligibility, policy, dependencies, learning state, CSP decision, size gate, and selected variant.

If CSS, JavaScript, or HTML remains unchanged, check cold versus warm state, source validators, parser support, integrity/nonce attributes, relative imports, source directives, script kind, later cascade boundaries, and aggregate transfer size.
Resource URLs remain unchanged when a `<base href>` is ambiguous, the URL is cross-origin or already shortest, the attribute is not resource-fetching, or the complete `srcset` cannot be validated.
If an image format is absent, check `Accept`, client hints, source magic, dimensions, codec capability, animation, quality policy, and whether the candidate was actually smaller.

For CSP failures, preserve the application policy and configure an accepted nonce or same-origin source rather than adding broad unsafe directives.
Immutable `/.laghu/` routes must remain same-origin, content-addressed, publicly cacheable, and backed by a ready catalog entry.

When output is semantically wrong, disable Laghu for the narrow affected route, preserve the original and transformed bytes plus headers, record `laghu explain`, and file a reproducible report.
