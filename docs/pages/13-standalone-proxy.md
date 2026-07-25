---
title: Standalone Proxy
nav_order: 6
permalink: /standalone-proxy/
---

# Standalone Proxy

The `laghu` executable applies the same `laghu-http` policy and transformation engine as the NGINX and Apache adapters to one configured HTTP origin. It is suitable when the origin cannot load a native module.

Start a development build with explicit listener, origin, cache, and worker-queue paths:

```bash
laghu \
  --listen 127.0.0.1:8080 \
  --origin http://127.0.0.1:8000 \
  --cache /var/cache/laghu \
  --worker-queue /run/laghu/jobs.queue
```

The origin must be an `http://` authority without credentials, a path prefix, a query, or a fragment. The proxy uses the balanced preset unless `--preset` or `--rewrite-level` selects another shared policy. `--allow-api`, `--image-quality`, and `--image-beacon` expose the corresponding process-wide settings.

The transport accepts one HTTP/1.0 or HTTP/1.1 request per connection and always closes both downstream and origin connections after that transaction. Four worker threads and 64 queued connections are the defaults. `--workers`, `--connection-queue`, `--connect-timeout`, and `--io-timeout` set bounded alternatives.

Request metadata is limited to 64 headers and 64 KiB, with 8 KiB request lines and header values. Fixed-length request bodies are accepted up to 1 MiB. Chunked requests, upgrades, `CONNECT`, `TRACE`, ambiguous framing, and malformed headers are rejected. Origin responses may use fixed-length, chunked, or connection-close framing and captured optimization inputs remain subject to their existing HTML, CSS, and image limits.

The proxy removes hop-by-hop headers, replaces `Host` with the configured origin authority, and does not add or trust forwarding headers. It performs no origin retries. A transformation, worker, queue, or cache failure preserves a complete origin response; an unavailable or malformed origin produces `502 Bad Gateway`.

The `/.laghu/image/<sha256>` and `/.laghu/css/<sha256>` routes are served locally through validated content-addressed lookup and are never forwarded. Enabling the image beacon also exposes its fixed script and bounded same-origin POST endpoint locally.

## Lifecycle and Probes

`SIGINT` and `SIGTERM` begin a graceful drain. The proxy closes its listener, returns `503` to connections still waiting in its bounded queue, and allows active requests to finish for 30 seconds. `--drain-timeout` accepts a value from 1 through 300 seconds. A second signal or deadline expiry interrupts active client and origin sockets, suppresses incomplete optimizer publication, joins every worker, and releases process resources. Windows console events use the same lifecycle; `--service` runs the executable under the Windows Service Control Manager.

`GET` and `HEAD` requests to `/.laghu/health` report process liveness. `/.laghu/ready` verifies writable cache publication locally and reports the optimizer queue as `ready` or `degraded`. Worker loss does not make the proxy unavailable because eligible traffic continues with original responses. Cache failure returns `503`. Neither probe contacts the origin.

The cache directory must exist and permit exclusive create, write, sync, and removal before the listener binds. A missing worker queue is accepted as degraded operation; an existing malformed or inaccessible queue prevents startup. Every mutable file remains beneath the configured cache or worker-queue paths.

## Operational Logs

The proxy writes JSON Lines to standard error. Lifecycle, readiness, overload, and transaction records include bounded operational fields. Transaction paths omit query strings, and records never contain client addresses, authorities, authorization, cookies, bodies, beacon payloads, cache paths, or queue paths. Logs remain local; the proxy sends no telemetry.

This transport is plaintext HTTP. TLS, persistent connections, origin pooling, HTTP/2, HTTP/3, and trusted-forwarded-header policy remain separate production-hardening capabilities.
