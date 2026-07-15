# Laghu

Laghu is a native NGINX content-optimization module. It is designed to rewrite
HTML, CSS, JavaScript, images, and fonts at the edge while remaining fail-open,
observable, and compatible with current NGINX releases.

This repository is an initial engineering skeleton. It establishes the native
module boundary, a server-independent policy library, tests, documentation, and
automation. It does not yet optimize response bodies.

## Product Shape

Laghu keeps three deliberate ownership boundaries:

- `laghu-core` owns server-independent configuration and optimization policy.
- `ngx_http_laghu_module` owns NGINX configuration, filter integration, and
  fail-open request handling.
- `docs/` owns installation, configuration, architecture, compatibility, and
  security documentation.

The future optimization worker, shared cache, CLI, and benchmark rail will use
the same core boundary rather than embedding heavy work in an NGINX event loop.

## Repository Map

- `libs/laghu-core/`: canonical native policy library and unit tests
- `modules/ngx_http_laghu_module/`: NGINX dynamic module integration
- `examples/`: runnable configuration examples
- `docs/`: product documentation site built with Jekyll and Just the Docs
- `.github/`: issue templates, workflows, dependency updates, and review rules
- `scripts/`: root-level build and validation commands

## Current Skeleton

The module currently:

- builds against stable and mainline NGINX
- accepts `laghu on|off` and `laghu preset <name>`
- supports `http`, `server`, and `location` inheritance
- conservatively bypasses ineligible statuses, private responses,
  authenticated requests, and unsupported content types
- emits an `X-Laghu` decision header when enabled
- passes the original body through unchanged

No transformation, variant cache, worker, metrics endpoint, CLI, or package is
claimed as implemented yet.

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

Use `scripts/run-in-docker scripts/run-all` for the Linux validation image.
Set `NGINX_VERSION` to build against another NGINX release.

## Documentation

Start with:

- [`docs/index.md`](docs/index.md)
- [`docs/pages/02-installation.md`](docs/pages/02-installation.md)
- [`docs/pages/03-configuration.md`](docs/pages/03-configuration.md)
- [`docs/pages/04-architecture.md`](docs/pages/04-architecture.md)
- [`docs/pages/05-development.md`](docs/pages/05-development.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and review expectations.
