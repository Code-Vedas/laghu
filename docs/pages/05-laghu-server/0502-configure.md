---
title: Configuration
parent: Laghu Server
nav_order: 2
permalink: /laghu-server/configure/
---

# Configure the Laghu Server

The standalone server accepts command-line options only.
`--listen`, `--origin`, `--cache`, and `--worker-queue` are required; duplicate or unknown options fail startup.

| Option | Accepted value | Default | Effect |
| --- | --- | --- | --- |
| `--listen HOST:PORT` | valid endpoint | required | Selects the client listener. |
| `--origin http[s]://HOST[:PORT]` | one origin without path/query/credentials | required | Selects the upstream origin. |
| `--cache PATH` | bounded path | required | Selects catalogs and immutable assets. |
| `--worker-queue PATH` | bounded path | required | Selects the image queue. |
| `--preset NAME` | supported preset | `balanced` | Selects policy; conflicts with `--rewrite-level`. |
| `--rewrite-level NAME` | supported rewrite level | unset | Selects policy; conflicts with `--preset`. |
| `--allow-api` | flag | off | Allows otherwise excluded API/GraphQL paths. |
| `--image-quality N` | `1..100` | codec default | Overrides lossy image quality. |
| `--image-beacon` | flag | off | Enables critical-image observations. |
| `--critical-css-beacon` | flag | off | Enables critical-CSS learning. |
| `--instrumentation-beacon` | flag | off | Enables RUM script injection when CSP permits. |
| `--instrumentation-sample-rate N` | `0..100` | `10` | Sets browser-side percentage sampling. |
| `--font-fetch-queue PATH` | bounded path | unset | Enables the font-fetch queue; requires provider config. |
| `--font-provider-config PATH` | valid provider file | unset | Enables providers; requires font queue. |
| `--javascript-queue PATH` | bounded path | unset | Enables the SWC queue. |
| `--javascript-target QUERY` | bounded Browserslist query | `defaults and supports es6-module and not dead` | Controls syntax lowering. |
| `--javascript-inline-limit N` | `0..65536` | `2048` | Caps JavaScript inlining. |
| `--javascript-outline-threshold N` | `1024..1048576` | `8192` | Selects inline scripts for outlining. |
| `--javascript-observation-config PATH` | valid observation file | unset | Adds exact third-party script candidates. |
| `--workers N` | `1..256` | `4` | Sets request worker threads. |
| `--connection-queue N` | `1..65536` | `64` | Bounds accepted connections waiting for workers. |
| `--connect-timeout SECONDS` | `1..300` | `5` | Bounds origin connection establishment. |
| `--io-timeout SECONDS` | `1..300` | `30` | Bounds client/origin I/O. |
| `--drain-timeout SECONDS` | `1..300` | `30` | Bounds graceful shutdown. |
| `--origin-ca-file PATH` | CA bundle path | platform trust | Overrides trust for HTTPS origins; invalid with HTTP. |
| `--forwarded-headers MODE` | `off`, `forwarded`, `x-forwarded`, `both` | `off` | Selects trusted forwarding syntax. |
| `--trusted-proxy CIDR` | canonical IPv4/IPv6 CIDR, repeatable to 64 | none | Trusts forwarding headers from matching peers; requires forwarding mode. |
| `--rum-store URI` | `memory:`, `local:`, supported Redis URI | `local:` | Selects RUM persistence/synchronization. |
| `--rum-store-local-snapshot PATH` | bounded path | `<cache>/rum.snapshot` | Selects last-known-good snapshot storage. |
| `--rum-store-client-library PATH` | hiredis library path | unset | Enables Redis/Valkey support. |
| `--rum-store-timeout MS` | `10..10000` | `100` | Bounds backend operations. |
| `--rum-store-ttl SECONDS` | `3600..2592000` | `604800` | Sets aggregate expiry. |
| `--rum-store-retry-limit N` | `0..10` | `3` | Bounds synchronization retries. |
| `--rum-store-sync-interval SECONDS` | `1..300` | `5` | Sets synchronization cadence. |
| `--rum-store-memory-limit BYTES` | `16KiB..1GiB`, size suffix accepted | `64MiB` | Bounds resident RUM records. |
| `--rum-store-pending-limit BYTES` | `16KiB..1GiB`, size suffix accepted | `16MiB` | Bounds pending deltas. |
| `--rum-store-required` | flag | off | Makes RUM initialization failure fatal. |
| `--help` | flag | n/a | Prints usage. |
| `--version` | flag | n/a | Prints version. |
| `--service` | Windows-only flag | off | Runs through the Windows service entry point. |

The proxy itself defaults enabled with `balanced`, unlike the disabled-by-default native modules.
Remote RUM synchronization requires verified `rediss://`; `redis://` is loopback-only and Memcached is unsupported.
