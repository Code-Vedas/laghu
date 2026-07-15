---
title: Configuration
nav_order: 3
permalink: /configuration/
---

# Configuration

The initial module exposes only configuration that has executable coverage.
Unsupported commands fail NGINX configuration validation instead of being
silently ignored.

## `laghu on|off`

Enables or disables Laghu for the current scope.

```nginx
laghu on;
```

- Context: `http`, `server`, `location`
- Default: `off`
- Inheritance: child scopes inherit their parent unless overridden

When enabled, the skeleton evaluates the response and emits an `X-Laghu`
decision. The response body remains unchanged.

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
| `bypass-status` | Response status is not eligible |
| `bypass-authorized` | Request carried authorization credentials |
| `bypass-private` | Response is marked `private` or `no-store` |
| `bypass-api` | Request path is API-classified and has no explicit override |
| `bypass-content-type` | Content type is outside the optimization surface |
| `bypass-error` | Policy metadata could not be inspected safely |

The header reports policy only. It does not claim that a transform occurred.

When multiple conditions apply, Laghu evaluates disabled configuration,
invalid policy data, status, authorization, private caching, API path, and
content type in that order.

## Scoped Configuration

```nginx
server {
  laghu on;

  location /unoptimized/ {
    laghu off;
  }

  location /api/public-assets/ {
    laghu allow_api on;
  }
}
```
