---
title: Development
nav_order: 5
permalink: /development/
---

# Development

Laghu development uses a canonical core library, a thin server module, central
product docs, and root-level automation.

## Repository Workflow

```bash
scripts/ci-install-dependencies
scripts/run-all
```

The full flow checks shell and C formatting when their tools are available,
builds the core with warnings as errors, runs CTest, compiles the NGINX dynamic
module, and builds the documentation site.

Focused commands:

```bash
scripts/run-lint-all
scripts/run-build-all
scripts/run-test-all
scripts/run-module-build-all
scripts/run-docs-build-all
```

Use the Linux validation image from macOS or another host platform:

```bash
scripts/run-in-docker scripts/run-all
```

## Core Work

Keep the public API in `libs/laghu-core/include/laghu/`. Implementation and
direct tests live under `src/` and `tests/` in the same package.

```bash
cmake -S . -B tmp/build -DCMAKE_BUILD_TYPE=Debug
cmake --build tmp/build --parallel
ctest --test-dir tmp/build --output-on-failure
```

## Module Work

NGINX integration belongs under `modules/ngx_http_laghu_module/`. Avoid leaking
NGINX types into the core library. Filter changes must preserve chain ordering,
pool ownership, and fail-open behavior.

Build against both the recorded stable and mainline versions before changing
the compatibility matrix.

## Documentation

```bash
cd docs
bundle install
bundle exec jekyll serve
```

The documentation theme is loaded from the pinned Just the Docs fork in
`docs/Gemfile`. Laghu-owned presentation hooks live in
`docs/_includes/head_custom.html` and `docs/_includes/footer_custom.html`;
light and dark theme entrypoints live under `docs/assets/css/`.

Docs are part of the product surface. Update them when commands, paths,
configuration, behavior, or support boundaries change.
