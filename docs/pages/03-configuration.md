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
`static`. The current skeleton parses and inherits the preset; filter-specific
behavior will arrive with the optimization milestones.

- Context: `http`, `server`, `location`
- Default: `balanced`

## Decision Header

An enabled location returns one of these `X-Laghu` values:

| Value | Meaning |
| --- | --- |
| `pass` | Response is eligible for the future optimization path |
| `bypass-status` | Response status is not eligible |
| `bypass-authorized` | Request carried authorization credentials |
| `bypass-private` | Response is marked `private` or `no-store` |
| `bypass-content-type` | Content type is outside the optimization surface |
| `bypass-error` | Policy metadata could not be inspected safely |

The header reports policy only. It does not claim that a transform occurred.

## Scoped Exclusion

```nginx
server {
  laghu on;

  location /api/ {
    laghu off;
  }
}
```

API and authenticated paths should remain explicitly disabled while the module
is in its scaffold stage.
