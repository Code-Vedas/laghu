---
title: Protocols, Cache, and RUM State
parent: Developer Guide
nav_order: 6
permalink: /developer-guide/runtime-contracts/
---

# Protocols, Cache, and RUM State

## Queue Contracts

Each queue is bounded, checksummed, capability-bearing, heartbeat-validated, and versioned independently.

Queue format v10 adds W3C trace context. Upgrade workers and adapters together, then recreate every queue file; an older mapping is rejected rather than being decoded with shifted fields.

`laghu-otel-export` consumes the dedicated trace queue. Set `LAGHU_OTEL_CACHE_PATH` to the configured image-cache path so its fixed-label worker metrics, queue health, failures, and duration histogram appear at the existing Prometheus endpoint. `LAGHU_OTEL_AUTHORIZATION` is optional and is sent only to the collector; neither value enters logs, metrics, or trace payloads.
Publishers use try-only submission and workers reject mismatched protocol, configuration digest, input bounds, or job kind.
Never add an unbounded field or a request-thread wait to a queue contract.

## Cache and Catalogs

Derived bytes are content-addressed and atomically published before a catalog references them.
Readers validate record version, checksum, policy/dependency keys, content metadata, expiry, and payload bounds.
Catalog corruption is a cache miss, not a reason to serve guessed output.

## RUM Engine

The in-memory engine is the request-facing source of truth.
The synchronizer rotates bounded pending deltas without holding the request mutex during backend I/O, merges returned aggregates, and publishes a portable last-known-good snapshot.

Redis/Valkey executes one fixed versioned Lua merge contract through `SCRIPT LOAD` and `EVALSHA`, retries one `NOSCRIPT` reload, validates the full batch before writes, and commits the batch marker last.
New record types require byte-for-byte C/Lua layout parity, saturation/max/union tests, duplicate-batch tests, concurrency convergence, wrong-type rejection, and failure/retry coverage.

## Compatibility Rule

Prefer explicit version rejection and relearning over mixed-version inference.
Credentials and backend endpoints never belong in policy keys, cache content, browser payloads, portable snapshots, or ordinary diagnostics.
