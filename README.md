# Laghu

Laghu is a free, open-source server-level content optimizer with native NGINX and Apache HTTP Server modules and a standalone reverse proxy. All three surfaces use the same policy, parsers, catalogs, workers, cache formats, and fail-open rules.

Cache extension is MIME-driven: `extend_cache_media` only handles administrator-allowlisted opaque media such as images, PDFs, fonts, audio, and video. CSS and JavaScript remain separate parser-aware resource filters; unsafe, private, uncached, oversized, or mismatched resources remain unchanged.

The image path uses an out-of-process libvips worker: a cold request serves the original while the adapter publishes a try-only job, and a later request can use a validated, strictly smaller cached variant. Large animated GIFs may become cached MP4/WebM video markup only after both artifacts validate; unavailable video encoding preserves the original GIF. Codec work never runs inside NGINX or Apache.

## Structured logs

Laghu emits privacy-safe laghu-log-v1 records for handled transactions, worker jobs, and lifecycle changes. Records never include query strings, headers, bodies, credentials, tokens, hosts, or cache keys.

The standalone server and workers write one JSON object per stderr line. NGINX and Apache preserve their configured native error-log sinks and write one laghu_json=<JSON> payload per Laghu record; extract that payload before JSON parsing.

Schema v1 fixes the common schema, timestamp, surface, component, and event fields. Transaction records add method, query-free path, status, decision, byte counts, duration, cache state, publication result, and a fixed failure reason. Worker records contain only job kind, outcome, byte counts, duration, and a fixed failure reason.

## Product Shape

Laghu keeps explicit ownership boundaries:

- `laghu-base` owns dependency-free bounded builders, ASCII operations, hashing, numeric parsing, and URL resolution.
- `laghu-markup` owns bounded HTML tags and attributes, `srcset`, and shared resource tokenization.
- `laghu-core` owns server-independent configuration and optimization policy.
- `laghu-image` owns explicit-codec image transforms, probes, and image-markup decisions.
- `laghu-runtime` owns caches, queues, catalogs, RUM, operational state, and transformation services.
- `laghu-http` solely owns normalized transactions, transformation sequencing, cache selection, headers, publication, budgets, and fail-open results.
- `laghu-libvips` owns isolated libvips execution and deadline enforcement.
- `laghu-html-refresh` owns verified-HTTPS stale-while-revalidate HTML refreshes for one configured origin and queue.
- `laghu-js-optimize` owns isolated SWC parsing, target lowering, compression, and local mangling.
- `ngx_http_laghu_module` owns NGINX configuration, filter integration, and fail-open request handling.
- `mod_laghu` owns Apache configuration, bucket-brigade integration, and fail-open request handling.
- `laghu` owns bounded HTTP/1.1 framing, origin forwarding, cancellation, and transport backpressure for origins without a native adapter.
- `docs/` owns installation, configuration, architecture, compatibility, and security documentation.

## Repository Map

- `libs/laghu-core/`: canonical native policy library and unit tests
- `libs/laghu-base/`: dependency-free bounded primitives
- `libs/laghu-markup/`: shared bounded markup tokenization
- `libs/laghu-image/`: server-independent image and image-markup pipeline
- `libs/laghu-runtime/`: shared queue and content-addressed cache protocol
- `libs/laghu-http/`: shared HTTP transaction engine and conformance tests
- `workers/laghu-libvips/`: asynchronous libvips worker
- `workers/laghu-html-refresh/`: HTTPS HTML stale-while-revalidate worker
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
- serves configured anonymous HTML cache entries fresh or stale and revalidates stale entries through a separate explicit-origin worker
- normalizes eligible document heads and places CSS through one bounded planner shared by NGINX and Apache
- safely collapses ordinary HTML whitespace, removes unprotected comments, unquotes safe values, and elides exact default MIME attributes through that same cold-original planner
- converts only conflict-free `Content-Language` metadata and emits bounded, deduplicated preload, third-party preconnect, and DNS-prefetch response hints from validated catalog state without fetching resources
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

Use `scripts/run-in-docker` for the default Linux test or `--all` for the complete local distro/server matrix. `--target debian-nginx-chrome-analysis` installs Chromium solely for the optional headless-browser analysis lane; production images do not include a browser. Set `NGINX_VERSION` to build another NGINX release.

Run the standalone proxy from a development build with one plaintext origin:

```bash
mkdir -p /tmp/laghu-conf/conf.d
cp servers/laghu/laghu.yaml.example /tmp/laghu-conf/laghu.yaml
# Edit paths and origin in /tmp/laghu-conf/laghu.yaml.
tmp/build/servers/laghu/laghu --config /tmp/laghu-conf/laghu.yaml
```

Runtime settings belong only in strict YAML. Like NGINX, `laghu` starts from its fixed main configuration,
`/etc/laghu/laghu.yaml`, then reads sibling `conf.d/*.yaml` fragments in lexical order. `--config PATH` is only
an administrative override for an alternate main configuration; it retains the same sibling fragment convention.
The origin and per-route `https://` targets use verified HTTPS. `origin_ca_file` adds a private CA to system trust; certificate and hostname verification cannot be disabled. Routes also support bounded health-aware HTTP failover plus FastCGI, uWSGI, and SCGI upstreams. Static delivery applies the same normalized Laghu transaction contract as proxied responses, with scoped gzip, SPA fallback, limits, CIDR access rules, Basic authentication, and rate limits resolved before requests are accepted.
The proxy exposes local health and readiness JSON beneath `/.laghu/`, drains active requests on shutdown, and emits privacy-bounded JSON Lines to standard error.

Server modules are architecture- and ABI-specific. Build each module against the target server ABI.

Windows is unsupported.

## Documentation

Start with:

- [`docs/index.md`](docs/index.md)
- [`Introduction`](docs/index.md)
- [`Architecture`](docs/pages/02-architecture.md)
- [`ngx-laghu`](docs/pages/03-ngx-laghu/index.md)
- [`mod-laghu`](docs/pages/04-mod-laghu/index.md)
- [`Laghu server`](docs/pages/05-laghu-server/index.md)
- [`Developer guide`](docs/pages/06-developer-guide/index.md)
- [`Security reporting`](docs/pages/07-security-reporting.md)
- [`Professional support`](docs/pages/08-professional-support.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and review expectations.

## Status

Query an authenticated Laghu endpoint without exposing its token:

```sh
laghu status https://laghu.example --token-file /etc/laghu/purge.token
laghu doctor https://laghu.example --token-file /etc/laghu/purge.token
```

`status` reads only an absolute, owner-only token file, sends it as
`X-Laghu-Purge-Token`, and queries `/.laghu/ready` then `/.laghu/stats`.
Use `--json` for the normalized result, `--timeout 1..30`, and `--ca-file` to
extend HTTPS trust. Exit codes distinguish CLI/token (2), authorization (3),
connection (4), malformed response (5), 503 (6), and other HTTP failures (7).

## Purge

Purge one cached source URL through the same authenticated endpoint:

```sh
laghu purge https://laghu.example/assets/site.css?tenant=blue --token-file /etc/laghu/purge.token
```

`purge` sends authenticated `PURGE` with the URL path and source query, follows no redirects, and prints the accepted artifact count. `--json`, `--timeout 1..30`, and HTTPS-only `--ca-file` use the same secure client rules as `status`; it never prints the token, request header, URL, or response body. Exit `8` means the bounded purge table is saturated; retry after cache maintenance. Other exit meanings match `status`, except `400` is invalid target and `404`/`405` remain other HTTP failures.
