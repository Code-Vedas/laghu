# Laghu

Laghu is a server-level content optimizer with equal native adapters for NGINX
and Apache HTTP Server. It rewrites responses at the edge while remaining
fail-open, observable, and compatible with current server releases.

The image path uses an out-of-process libvips worker: a cold request serves the
original while the adapter publishes a try-only job, and a later request can
use a validated, strictly smaller cached variant. Codec work never runs inside
NGINX or Apache.

## Product Shape

Laghu keeps six deliberate ownership boundaries:

- `laghu-core` owns server-independent configuration and optimization policy.
- `laghu-image` owns explicit-codec image transforms and markup primitives.
- `laghu-runtime` owns the bounded queue and atomic disk publication protocol.
- `laghu-libvips` owns isolated libvips execution and deadline enforcement.
- `ngx_http_laghu_module` owns NGINX configuration, filter integration, and
  fail-open request handling.
- `mod_laghu` owns Apache configuration, bucket-brigade integration, and
  fail-open request handling.
- `docs/` owns installation, configuration, architecture, compatibility, and
  security documentation.

## Repository Map

- `libs/laghu-core/`: canonical native policy library and unit tests
- `libs/laghu-image/`: server-independent image and image-markup pipeline
- `libs/laghu-runtime/`: shared queue and content-addressed cache protocol
- `workers/laghu-libvips/`: asynchronous libvips worker
- `modules/ngx_http_laghu_module/`: NGINX dynamic module integration
- `modules/mod_laghu/`: Apache HTTP Server output-filter integration
- `examples/`: runnable configuration examples
- `docs/`: product documentation site built with Jekyll and Just the Docs
- `.github/`: issue templates, workflows, dependency updates, and review rules
- `scripts/`: root-level build and validation commands
- `packaging/`: Debian, RPM, NGINX-module, systemd, and bundled-container inputs

## Current Runtime

The module currently:

- builds against stable and mainline NGINX
- accepts `laghu on|off`, `laghu preset <name>`,
  `laghu rewrite_level <name>`, `laghu allow_api on|off`, inherited image
  quality, queue, and cache configuration
- supports `http`, `server`, and `location` inheritance
- resolves every preset to a tested filter-family and safety policy
- resolves passthrough, core, bandwidth, all, and experimental rewrite levels
  without claiming their pending filters execute
- conservatively bypasses API paths, ineligible statuses, private responses,
  authenticated requests, and unsupported content types
- provides stable SHA-256 variant keys and an original-preserving,
  never-larger candidate-selection contract for future transforms
- probes explicit JPEG, PNG, GIF, animated-image, and WebP operations
- queues bounded image jobs without waiting and publishes cache files atomically
- normalizes eligible document heads and places CSS through one bounded planner
  shared by NGINX and Apache
- safely collapses ordinary HTML whitespace, removes unprotected comments,
  unquotes safe values, and elides exact default MIME attributes through that
  same cold-original planner
- emits `pass`, `image-hit`, or a specific fail-open bypass in `X-Laghu`
- preserves the original on a cold miss, backend loss, queue contention,
  malformed input, timeout, invalid output, or non-smaller output

The roadmap tracks the remaining HTML, CSS, JavaScript, cache-control,
administration, purging, and metrics filters individually.

## Local Development

Run the repository validation flow:

```bash
scripts/run-all
```

Focused commands are available through `make` or directly:

```bash
make build
make test
make lint
make module
make docs
```

Use `scripts/run-in-docker` for the default Linux test or `--all` for the
complete local distro/server matrix. Set `NGINX_VERSION` to build another
NGINX release.

Users install one server offering: `ngx-laghu` or `mod-laghu`. Each pulls the
internal `laghu-libvips` service and loads its adapter with optimization off.
Server modules are architecture- and ABI-specific and are rebuilt whenever the
corresponding server ABI changes.

Version tags assemble Linux x86_64/arm64 raw modules for the pinned stable and
mainline NGINX releases, attach deb/rpm package sets to the GitHub release, and
publish separate multi-architecture images to `ghcr.io/code-vedas/ngx-laghu`
and `ghcr.io/code-vedas/mod-laghu`.

## Documentation

Start with:

- [`docs/index.md`](docs/index.md)
- [`docs/pages/02-installation.md`](docs/pages/02-installation.md)
- [`docs/pages/03-configuration.md`](docs/pages/03-configuration.md)
- [`docs/pages/04-architecture.md`](docs/pages/04-architecture.md)
- [`docs/pages/05-development.md`](docs/pages/05-development.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and review expectations.
