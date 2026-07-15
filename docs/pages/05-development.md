---
title: Development
nav_order: 5
permalink: /development/
---

# Development

Laghu development uses canonical shared libraries, equal NGINX and Apache
adapters, central product docs, and root-level automation.

## Repository Workflow

```bash
scripts/ci-install-dependencies
scripts/run-all
```

The full flow checks shell and C formatting when their tools are available,
builds with warnings as errors, runs CTest, compiles both server adapters, runs
cold/warm smoke coverage, and builds the documentation site.

Focused commands:

```bash
scripts/run-lint-all
scripts/run-build-all
scripts/run-test-all
scripts/run-module-build-all
scripts/run-apache-module-smoke-all
scripts/run-macos-service-smoke # macOS only
scripts/run-homebrew-smoke # macOS only
scripts/run-docs-build-all
```

Use the Linux validation image from macOS or another host platform:

```bash
scripts/run-in-docker
scripts/run-in-docker --all
```

## Core Work

Keep the public API in `libs/laghu-core/include/laghu/`. Implementation and
direct tests live under `src/` and `tests/` in the same package.

```bash
cmake -S . -B tmp/build -DCMAKE_BUILD_TYPE=Debug
cmake --build tmp/build --parallel
ctest --test-dir tmp/build --output-on-failure
```

Use `LAGHU_WITH_VIPS=ON`, `AUTO`, and `OFF` to validate the enabled, optional,
and stub builds. CI also runs the dependency-free image parsers under ASan,
UBSan, and libFuzzer. Image tests generate deterministic JPEG, PNG, static GIF,
animated GIF, and WebP fixtures through explicit libvips savers.

## Module Work

NGINX integration belongs under `modules/ngx_http_laghu_module/`; Apache
integration belongs under `modules/mod_laghu/`. Avoid leaking either server's
types into shared libraries. Preserve NGINX chain/pool ownership and Apache
brigade/pool ownership, including metadata, `FLUSH`, and `EOS` handling.

Build against both the recorded stable and mainline versions before changing
the compatibility matrix.

CI runs stable/mainline NGINX and Apache 2.4 adapter smoke tests. Linux, macOS,
and Windows x86_64/arm64 run the shared runtime and `laghu-libvips` tests using
their native platform backend.

## Packaging Work

Debian and RPM builds use the distribution NGINX and Apache development ABIs.
Do not publish a generic module without its corresponding server dependency.
Homebrew builds against its selected formula, and Windows installers contain
matched server builds.

Package validation installs `ngx-laghu` and `mod-laghu`, runs `nginx -t` and
`httpd -t`, probes `laghu-libvips`, and exercises both container images.

### Enterprise Linux 9 libvips source

The production Codevedas RPM repository will publish Codevedas-built libvips
and codec dependency RPMs from pinned, reviewed source RPMs. Users must not
need to enable Remi or another third-party repository after enabling the Laghu
repository. Rocky and Alma development containers currently use Remi Safe as
a bootstrap source because EL9 base repositories do not provide the required
libvips baseline; package installation verifies the selected repository and
RPM signatures. Remi artifacts are test inputs, not the production dependency
boundary.

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
