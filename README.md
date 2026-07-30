# Laghu

Laghu is a free, open-source server-level content optimizer with native NGINX and Apache HTTP Server modules and a standalone reverse proxy. All three surfaces use the same policy, parsers, catalogs, workers, cache formats, and fail-open rules.

The image path uses an out-of-process libvips worker: a cold request serves the original while the adapter publishes a try-only job, and a later request can use a validated, strictly smaller cached variant. Codec work never runs inside NGINX or Apache.

## Product Shape

Laghu keeps ten deliberate ownership boundaries:

- `laghu-core` owns server-independent configuration and optimization policy.
- `laghu-image` owns explicit-codec image transforms and markup primitives.
- `laghu-runtime` owns the bounded queue and atomic disk publication protocol.
- `laghu-http` owns the bounded, server-neutral HTTP transaction contract and executable response orchestration.
- `laghu-libvips` owns isolated libvips execution and deadline enforcement.
- `laghu-js-optimize` owns isolated SWC parsing, target lowering, compression, and local mangling.
- `ngx_http_laghu_module` owns NGINX configuration, filter integration, and fail-open request handling.
- `mod_laghu` owns Apache configuration, bucket-brigade integration, and fail-open request handling.
- `laghu` owns bounded HTTP/1.1 framing, origin forwarding, cancellation, and transport backpressure for origins without a native adapter.
- `docs/` owns installation, configuration, architecture, compatibility, and security documentation.

## Repository Map

- `libs/laghu-core/`: canonical native policy library and unit tests
- `libs/laghu-image/`: server-independent image and image-markup pipeline
- `libs/laghu-runtime/`: shared queue and content-addressed cache protocol
- `libs/laghu-http/`: shared HTTP transaction engine and conformance tests
- `workers/laghu-libvips/`: asynchronous libvips worker
- `workers/laghu-js-optimize/`: native Rust SWC queue worker
- `modules/ngx_http_laghu_module/`: NGINX dynamic module integration
- `modules/mod_laghu/`: Apache HTTP Server output-filter integration
- `servers/laghu/`: portable standalone HTTP/1.1 reverse proxy
- `examples/`: runnable configuration examples
- `docs/`: product documentation site built with Jekyll and Just the Docs
- `.github/`: issue templates, workflows, dependency updates, and review rules
- `scripts/`: root-level build and validation commands
- `packaging/`: Debian, RPM, NGINX-module, systemd, and bundled-container inputs

## Runtime Behavior

The three adapters:

- share equivalent NGINX and Apache configuration, policy resolution, response eligibility, and failure behavior
- build as an NGINX dynamic module against the recorded stable and mainline releases and as an Apache HTTP Server 2.4 output filter
- expose native server inheritance through NGINX and Apache and explicit process-wide configuration through the standalone proxy
- resolve every preset to a tested filter-family and safety policy
- resolve passthrough, core, bandwidth, all, and experimental rewrite levels
- conservatively bypasses API paths, ineligible statuses, private responses, authenticated requests, and unsupported content types
- provides stable SHA-256 variant keys and an original-preserving, never-larger candidate-selection contract
- probes explicit JPEG, PNG, GIF, animated-image, and WebP operations
- queues bounded image jobs without waiting and publishes cache files atomically
- normalizes eligible document heads and places CSS through one bounded planner shared by NGINX and Apache
- safely collapses ordinary HTML whitespace, removes unprotected comments, unquotes safe values, and elides exact default MIME attributes through that same cold-original planner
- converts only conflict-free `Content-Language` metadata and emits bounded, deduplicated preload and DNS-prefetch response hints from validated catalog state without fetching resources
- emits `pass`, `image-hit`, or a specific fail-open bypass in `X-Laghu`
- preserves the original on a cold miss, backend loss, queue contention, malformed input, timeout, invalid output, or non-smaller output

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

Use `scripts/run-in-docker` for the default Linux test or `--all` for the complete local distro/server matrix. Set `NGINX_VERSION` to build another NGINX release.

Run the standalone proxy from a development build with one plaintext origin:

```bash
tmp/build/servers/laghu/laghu \
  --listen 127.0.0.1:8080 \
  --origin http://127.0.0.1:8000 \
  --cache /tmp/laghu-cache \
  --worker-queue /tmp/laghu.queue \
  --drain-timeout 30
```

The origin may use verified HTTPS. `--origin-ca-file` adds a private CA to system trust; certificate and hostname verification cannot be disabled. Forwarding headers are stripped unless `--forwarded-headers` explicitly enables deterministic output, and only repeated `--trusted-proxy` CIDRs may contribute an existing chain. The proxy exposes local health and readiness JSON beneath `/.laghu/`, drains active requests on shutdown, and emits privacy-bounded JSON Lines to standard error. `scripts/run-proxy-rootless` validates it as a non-root process with a read-only container filesystem.

Server modules are architecture- and ABI-specific. Build each module against the target server ABI.

## Documentation

Start with:

- [`docs/index.md`](docs/index.md)
- [`docs/pages/02-installation.md`](docs/pages/02-installation.md)
- [`docs/pages/03-configuration.md`](docs/pages/03-configuration.md)
- [`docs/pages/04-architecture.md`](docs/pages/04-architecture.md)
- [`docs/pages/05-development.md`](docs/pages/05-development.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and review expectations.
