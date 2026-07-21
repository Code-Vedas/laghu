# Changelog

All notable changes to Laghu will be documented in this file.

The project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Initial `laghu-core` policy library and unit tests.
- Native NGINX and Apache HTTP Server adapters with configuration inheritance.
- Resolved preset policies, default API-path exclusion with scoped overrides,
  and API bypass decisions.
- Native passthrough, core, bandwidth, all, and experimental rewrite-level
  policies with hierarchical selector replacement.
- Dependency-free SHA-256 content/variant keys and an original-preserving,
  never-larger candidate-selection contract; variant keys now use the version
  4 selector-, quality-, service-, and image-plan-aware encoding.
- Server-independent libvips image pipeline, isolated `laghu-libvips`,
  capability-aware bounded shared queue, and atomic content-addressed cache.
- Cold-original/warm-variant server delivery with inherited image quality and
  runtime paths, explicit image backend bypass, and strict fail-open behavior.
- Image markup primitives for dimensions, responsive sources, native lazy
  loading, data-URI/LQIP inlining, deduplication, and CSS sprite coordinates.
- Queue protocol v4 batched 1x/2x geometry plans, a checksummed image catalog,
  bounded same-origin HTML discovery, CSP and page-bundle gates, and inherited
  image beacon/inline/metadata settings for NGINX and Apache.
- Cold-original/warm-rewritten HTML image delivery in both adapters, including
  dimensions, responsive variants, native lazy loading, final/LQIP inlining,
  inline deduplication, client hints, and privacy-bounded beacon learning.
- Official packaging inputs requiring libvips 8.15+, hardened service/runtime
  directory definitions, sanitizer coverage, and an image parser fuzz harness.
- `ngx-laghu` and `mod-laghu` Debian/RPM package families with the internal
  `laghu-libvips` dependency and matching NGINX/Apache ABI validation.
- Separate NGINX and Apache containers with matching adapters, libvips runtime,
  and native x86_64/arm64 smoke coverage.
- POSIX and Win32 queue/cache backends with native process isolation and
  Linux, macOS, and Windows x86_64/arm64 tests.
- Tag-driven GitHub release assets for stable/mainline raw modules and deb/rpm
  packages, plus a multi-architecture bundled image in GHCR.
- Local Docker test matrix for Debian, Ubuntu, Fedora, Rocky, and AlmaLinux
  across NGINX, Apache, amd64, and arm64 targets.
- Product documentation and GitHub project automation.
- Homebrew lifecycle validation for both adapters and the shared service,
  including exact NGINX build-signature matching and reinstall-safe operator
  configuration.
