# Contributing to Laghu

Thank you for contributing. Laghu contains a server-independent C library, a
thin NGINX integration module, product documentation, and repository automation.

## Repository Layout

| Path | Purpose |
| --- | --- |
| `libs/laghu-core/` | Native policy library and unit tests |
| `modules/ngx_http_laghu_module/` | NGINX configuration and filter hooks |
| `docs/` | Product documentation site |
| `.github/` | Workflows, issue templates, and review guidance |
| `scripts/` | Root-level validation helpers |

## Development Baseline

Use CMake 3.20 or newer and a C11 compiler. Building the NGINX module also
requires `curl`, `make`, PCRE2, zlib, and the platform's standard build tools.
Ruby and Bundler are required for the documentation site.

```bash
scripts/ci-install-dependencies
scripts/run-all
```

The default module build targets the stable version recorded by
`scripts/build-nginx-module`. Override it when working on compatibility:

```bash
NGINX_VERSION=1.31.2 scripts/run-module-build-all
```

## Ownership Rules

- Keep NGINX-specific types and lifecycle code under `modules/`.
- Put reusable policy, cache contracts, and transform orchestration under
  `libs/laghu-core/`.
- Heavy or blocking optimization work must not run in the NGINX event loop.
- Every failure path must preserve the original response.
- Do not document a filter, directive, endpoint, package, or compatibility
  target as available before an executable test proves it.
- Add direct tests for behavior owned by a changed source file.

## Pull Requests

Before opening a pull request:

1. Use a meaningful branch prefix such as `feat/`, `bugfix/`, `docs/`, or `ci/`.
2. Run the focused checks and `scripts/run-all` when practical.
3. Update product docs when commands, configuration, behavior, or compatibility
   changes.
4. Complete the pull request template and call out fail-open and performance
   implications.

## Security

Follow [SECURITY.md](SECURITY.md) for vulnerability reports. Do not publish
exploit details in a public issue.

