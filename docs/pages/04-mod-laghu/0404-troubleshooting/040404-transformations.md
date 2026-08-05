---
title: Filters and CSP
parent: Troubleshooting
grand_parent: mod-laghu
nav_order: 4
permalink: /mod-laghu/troubleshooting/transformations/
---

# mod-laghu Filters and CSP

Use `X-Laghu`, response cache/CSP headers, and authenticated metrics to distinguish cold state and bounded failure classes.

Check filter ordering with compression, proxying, SSI, application filters, and content generators.
Laghu must receive a bounded complete representation in an eligible encoding and must update entity headers atomically with the body.
Resource URLs remain unchanged when a `<base href>` is ambiguous, the URL is cross-origin or already shortest, the attribute is not resource-fetching, or the complete `srcset` cannot be validated.

Laghu intersects every enforcing CSP header and bounded CSP meta policy; report-only headers are ignored for enforcement. Directive precedence and sources including `data:`, nonces, hashes, `'unsafe-inline'`, `'unsafe-hashes'`, and `'strict-dynamic'` are evaluated before each affected filter. A matching application nonce is preserved only on the element being transformed. Hash-only inline content and integrity-bearing elements remain byte-identical, and `'strict-dynamic'` prevents unauthorized parser-script injection.

Preserve strict CSP. Use application-owned nonces or permitted same-origin immutable routes instead of adding broad unsafe sources. Laghu never changes CSP headers or generates nonces; malformed, excessive, or ambiguous policies disable only the affected filter.

If output changes application behavior, disable Laghu only in the affected `<Location>` or virtual host, retain original/transformed fixtures and headers, and report the effective module/filter configuration with reproduction steps.
