---
title: Configuration
nav_order: 3
permalink: /configuration/
---

# Configuration

Both server adapters expose the same configuration semantics. NGINX uses the
lowercase `laghu` directive with semicolons; Apache uses `Laghu` without
semicolons. Unsupported settings fail server configuration validation.

| NGINX | Apache HTTP Server |
| --- | --- |
| `laghu on;` | `Laghu On` |
| `laghu preset balanced;` | `Laghu Preset balanced` |
| `laghu rewrite_level core;` | `Laghu RewriteLevel core` |
| `laghu allow_api off;` | `Laghu AllowApi Off` |
| `laghu image_quality 82;` | `Laghu ImageQuality 82` |
| `laghu image_beacon off;` | `Laghu ImageBeacon Off` |
| `laghu image_inline_limit 2048;` | `Laghu ImageInlineLimit 2048` |
| `laghu image_metadata_limit 10000;` | `Laghu ImageMetadataLimit 10000` |
| `laghu image_metadata_ttl 7d;` | `Laghu ImageMetadataTtl 7d` |

## `laghu on|off`

Enables or disables Laghu for the current scope.

```nginx
laghu on;
```

- Context: `http`, `server`, `location`
- Default: `off`
- Inheritance: child scopes inherit their parent unless overridden

When enabled, Laghu evaluates the response and may enqueue eligible images or
serve an already-published image variant.

## `laghu preset <name>`

Selects an optimization policy preset:

```nginx
laghu preset balanced;
```

Accepted names are `safe`, `balanced`, `aggressive`, `ecommerce`, `blog`, and
`static`. Each resolves to a filter-family policy used by the future transform
pipeline. A family being selected does not mean its pending transforms are
implemented yet.

- Context: `http`, `server`, `location`
- Default: `balanced`

| Preset | Selected policy |
| --- | --- |
| `safe` | Lossless image work, metadata removal, and dimensions; no lossy or structural changes |
| `balanced` | Safe plus modern/responsive images, lazy loading, minification, hints, and cache extension |
| `aggressive` | Every defined family except static-only immutable caching |
| `ecommerce` | Image-aware markup and CSS optimization without HTML/JavaScript minification or script reordering |
| `blog` | Balanced plus critical CSS, small-resource inlining, and JavaScript deferral |
| `static` | Aggressive plus immutable content-hash caching policy |

Universal authorization, privacy, fail-open, deterministic-output, and
never-larger safeguards cannot be disabled by a preset.

## `laghu rewrite_level <name>`

Selects a compatibility-oriented filter-family policy:

```nginx
laghu rewrite_level core;
```

| Level | Selected policy |
| --- | --- |
| `passthrough` | No filter families; the enabled module always preserves the original |
| `core` | The current balanced, recommended-default family set |
| `bandwidth` | Image recompression/modern formats, metadata removal, text minification, and cache extension without structural, inline, combine, critical-CSS, or script-order changes |
| `all` | Every currently defined family with expansive permissions |
| `experimental` | `all` plus explicit permission for experimental filters |

- Context: `http`, `server`, `location`
- Inheritance: child scopes inherit their parent unless overridden
- Default: no rewrite level; the default selector remains `preset balanced`

`laghu preset` and `laghu rewrite_level` are alternative policy selectors and
cannot appear together in one scope. A child scope may replace an inherited
preset with a rewrite level, or an inherited rewrite level with a preset.

Rewrite levels select policy only. Filters that have not been implemented and
validated remain unavailable regardless of which level selects their family.

## Image Runtime Directives

```nginx
laghu image_quality 82;
laghu image_beacon off;
laghu image_inline_limit 2048;
laghu image_metadata_limit 10000;
laghu image_metadata_ttl 7d;
laghu worker_queue /run/laghu/jobs.queue;
laghu image_cache /var/cache/laghu/images;
```

All image runtime directives inherit through `http`, `server`, and `location`. Quality
must be `1..100`. Without an override, ecommerce uses 85;
balanced/core/blog/bandwidth use 82; and aggressive/static/all/experimental use
75. Safe permits no lossy output, so an inherited quality does not relax it.

The queue and cache paths default to the values above. `laghu-libvips` must
have write access; the selected server needs queue access and read access to
published cache entries.

Beaconing is inherited and disabled by default. Inline payloads default to a
2 KiB maximum and may be configured from `0..16384`. Image metadata defaults
to 10,000 entries with a seven-day TTL; accepted TTLs range from `1h..30d`.
The same settings are exposed by Apache as `Laghu ImageBeacon`,
`Laghu ImageInlineLimit`, `Laghu ImageMetadataLimit`, and
`Laghu ImageMetadataTtl`.

Cold HTML is preserved while Laghu discovers image dependencies. Warm HTML is
rewritten only after every dependency is ready or terminally excluded. When
enabled, `image_beacon` serves a fixed same-origin script and accepts bounded
same-origin JSON observations at `/.laghu/beacon/images`; it does not store
cookies, IP addresses, client identifiers, or page content. The reserved
`/.laghu/image/<hash>` route serves only validated content-addressed variants.

## `laghu allow_api on|off`

Laghu bypasses `/api`, `/api/...`, `/graphql`, and `/graphql/...` by default.
The comparison is case-sensitive and observes path-segment boundaries, so
`/apiary` and `/graphql-ui` are not classified as API paths.

Use a narrow override only when an API-namespaced location serves optimizable
HTML, CSS, JavaScript, images, or fonts:

```nginx
location /api/public-assets/ {
  laghu allow_api on;
}
```

- Context: `http`, `server`, `location`
- Default: `off`
- Inheritance: child scopes inherit their parent unless overridden

The override disables only path classification. Authorization, `private`,
`no-store`, status, and content-type safeguards still apply.

## Decision Header

An enabled location returns one of these `X-Laghu` values:

| Value | Meaning |
| --- | --- |
| `pass` | Response is eligible for the future optimization path |
| `bypass-passthrough` | Passthrough level intentionally selected no filter families |
| `bypass-status` | Response status is not eligible |
| `bypass-authorized` | Request carried authorization credentials |
| `bypass-private` | Response is marked `private` or `no-store` |
| `bypass-api` | Request path is API-classified and has no explicit override |
| `bypass-content-type` | Content type is outside the optimization surface |
| `bypass-encoded` | Origin bytes already carry a content encoding and cannot be safely substituted |
| `bypass-image-backend` | No selected transform is supported by the published worker capability mask |
| `image-hit` | A strong-validator lookup selected an atomic cached image variant |
| `bypass-error` | Policy metadata could not be inspected safely |

Only `image-hit` claims variant delivery. `pass` on an image cold miss means the
original was streamed and an optimization job may have been published.

When multiple conditions apply, Laghu evaluates disabled configuration,
invalid policy data, intentional passthrough, status, authorization, private
caching, API path, and content type in that order.

## Scoped Configuration

```nginx
server {
  laghu on;

  location /unoptimized/ {
    laghu rewrite_level passthrough;
  }

  location /api/public-assets/ {
    laghu allow_api on;
  }
}
```
