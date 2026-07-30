---
title: Development
nav_order: 5
permalink: /development/
---

# Development

Laghu development uses canonical shared libraries, equal NGINX and Apache module boundaries, central product documentation, and root-level automation.

## Repository Workflow

```bash
scripts/ci-install-dependencies
scripts/run-all
```

The full flow checks shell and C formatting when their tools are available, builds with warnings as errors, runs CTest, compiles both server adapters, runs cold/warm smoke coverage, and builds the documentation site.

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

## Shared Library Work

Keep each public C API under its library's `include/laghu/` directory. Implementation and direct tests live under `src/` and `tests/` in the same package. `laghu-http` must remain free of NGINX, APR, socket, TLS, and event-loop types; its conformance suite must link only the shared Laghu libraries.

```bash
cmake -S . -B tmp/build -DCMAKE_BUILD_TYPE=Debug
cmake --build tmp/build --parallel
ctest --test-dir tmp/build --output-on-failure
```

Use `LAGHU_WITH_VIPS=ON`, `AUTO`, and `OFF` to validate the enabled, optional, and stub builds. CI also runs the dependency-free image parsers under ASan, UBSan, and libFuzzer. Image tests generate deterministic JPEG, PNG, static GIF, animated GIF, and WebP fixtures through explicit libvips savers.

## Module Work

NGINX integration belongs under `modules/ngx_http_laghu_module/`; Apache integration belongs under `modules/mod_laghu/`. Avoid leaking either server's types into shared libraries. Preserve NGINX chain/pool ownership and Apache brigade/pool ownership, including metadata, `FLUSH`, and `EOS` handling.

Build against both the recorded stable and mainline versions before changing the compatibility matrix.

CI runs stable/mainline NGINX and Apache 2.4 adapter smoke tests. Linux, macOS, and native Windows x86_64/arm64 runners execute the shared runtime and `laghu-libvips` tests using their platform backend.

## Native Windows Validation

Windows validation has one supported entry point. `run.ps1` delegates focused implementation work to scripts under `packaging/windows/private`; those private scripts are not a developer or CI interface.

```powershell
packaging/windows/run.ps1 Bootstrap -Architecture x64
packaging/windows/run.ps1 All -Architecture x64
```

`Bootstrap` installs the pinned codec and build prerequisites. `Build`, `Test`, and `Package` run individual phases, while `All` validates the PowerShell sources and executes every phase. `Manifests` combines native x64 and ARM64 installer artifacts into local Winget fixtures.

Use `-Architecture arm64` only on a native Windows ARM64 runner. Native x64 and ARM64 validation covers the codec-enabled and fail-open workers, Job Object isolation, matched servers, installer lifecycle, and service cleanup. `windows-sources.psd1` is the source-version and digest manifest for NGINX, Apache HTTP Server, APR, APR-util, OpenSSL, PCRE2, and zlib.

The Windows build-tool bootstrap verifies its native Strawberry Perl package by SHA-256 and verifies the Inno Setup publisher signature before installation.

The matched NGINX root contains a statically included Laghu adapter. The matched Apache root contains `mod_laghu.so` built against that root's HTTP Server and APR import libraries. `laghu-build.json` records the server version, architecture, compiler, Laghu revision, source archives, source hashes, and adapter artifact hashes; installer construction rejects a mismatched manifest, machine type, server binary, or Apache module.

## Packaging Work

Debian and RPM builds use the distribution NGINX and Apache development ABIs. Do not publish a generic module without its corresponding server dependency. Homebrew builds against its selected formula, and Windows installers contain matched server builds.

Package validation installs `ngx-laghu` and `mod-laghu`, runs `nginx -t` and `httpd -t`, probes `laghu-libvips`, and exercises both container images.

## Documentation

```bash
cd docs
bundle install
bundle exec jekyll serve
```

The documentation theme is loaded from the pinned Just the Docs fork in `docs/Gemfile`. Laghu-owned presentation hooks live in `docs/_includes/head_custom.html` and `docs/_includes/footer_custom.html`; light and dark theme entrypoints live under `docs/assets/css/`.

Docs are part of the product surface. Update them when commands, paths, configuration, behavior, or support boundaries change.
