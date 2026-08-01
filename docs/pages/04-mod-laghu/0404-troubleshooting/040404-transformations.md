---
title: Filters and CSP
parent: Troubleshooting
grand_parent: mod-laghu
nav_order: 4
permalink: /mod-laghu/troubleshooting/transformations/
---

# mod-laghu Filters and CSP

Use `laghu explain` to distinguish cold state, parser rejection, unsupported attributes, CSP, integrity, learning quorum, dependency readiness, and transfer-size rejection.

Check filter ordering with compression, proxying, SSI, application filters, and content generators.
Laghu must receive a bounded complete representation in an eligible encoding and must update entity headers atomically with the body.
Resource URLs remain unchanged when a `<base href>` is ambiguous, the URL is cross-origin or already shortest, the attribute is not resource-fetching, or the complete `srcset` cannot be validated.

Preserve strict CSP.
Use application-owned nonces or permitted same-origin immutable routes instead of adding broad unsafe sources.

If output changes application behavior, disable Laghu only in the affected `<Location>` or virtual host, retain original/transformed fixtures and headers, and report the effective module/filter configuration with reproduction steps.
