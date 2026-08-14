---
title: Security Reporting
nav_order: 7
permalink: /security-reporting/
---

# Security Reporting

Do not disclose a suspected vulnerability in a public issue.
Use [GitHub private vulnerability reporting](https://github.com/Code-Vedas/laghu/security/advisories/new); if that is unavailable, contact <admin@codevedas.com>.
Include the affected revision or release, deployment surface, reproduction steps, observed impact, and any proposed mitigation, but no production credentials or unrelated customer data.

Laghu acknowledges private reports within five business days, triages severity and affected surfaces
within ten business days, and sends a private status update at least every ten business days until
resolution. Fixes remain in a private advisory during any embargo. A public advisory follows a released
fix and includes affected/fixed versions, mitigation, and a CVE when eligible. Supported branches are
only those explicitly identified by current release notes and advisories.

## Security Model

Laghu is disabled by default in both native modules, bypasses authorization-bearing requests and private or `no-store` responses, and excludes API paths unless explicitly allowed. Transformations are bounded, validated, and fail open. Heavy codec and network work is isolated from web-server processes.

External font CSS fetching permits only administrator-configured exact HTTPS hosts and paths, validates DNS and redirects, and rejects private or reserved destinations. Redis credentials stay in administrator configuration and never enter browser output, policy keys, snapshots, or normal diagnostics. Browser instrumentation accepts bounded same-origin reports and stores aggregate opaque records rather than raw events or identifiers.
LCP reports contain only a bounded media ordinal, fixed kind and viewport/theme buckets, and a SHA-256 resource identity. The runtime rejects identities outside its current server-owned template inventory and never stores or exports the candidate URL, selector, text, class, attribute values, or template hash as an operational label.

Cache administration is off by default. When enabled, purge and detailed statistics require both a direct-peer CIDR match and the `X-Laghu-Purge-Token` value from an absolute, access-restricted token file. Do not place tokens in URLs, configuration values, logs, or responses. Keep `/.laghu/stats` on a private operations network. Purges invalidate shared metadata immediately and reclaim backend files asynchronously; they never delete CDN objects or unrelated files.

Review the product configuration before deployment: [`ngx-laghu`](/ngx-laghu/configure/), [`mod-laghu`](/mod-laghu/configure/), or [`laghu` server](/laghu-server/configure/).
