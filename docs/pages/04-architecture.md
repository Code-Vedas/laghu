---
title: Architecture
nav_order: 4
permalink: /architecture/
---

# Architecture

Laghu separates server integration from optimization policy and expensive execution.

```text
NGINX response filters ---- ngx-laghu ----+
                                          |
Apache bucket brigades --- mod-laghu -----+--> policy, parsers, and catalogs
                                                       |
                                            bounded shared queue
                                                       |
                                                laghu-libvips
                                                       |
                                           atomic disk variant cache
```

## NGINX Module

The module owns directive parsing, configuration inheritance, response filter registration, NGINX memory-pool use, and delivery of the original chain. It must not perform blocking network or codec work in the event loop.

## Apache Module

`mod_laghu` is an Apache 2.4 resource output filter. It retains request state across repeated brigade calls, forwards metadata and `FLUSH` buckets, detects `EOS`, and uses nonblocking bucket reads while copying a bounded cold response. It shares every policy and delivery gate with `ngx-laghu`.

## Core Library

The C library owns policy shared by both modules and the worker. Its responsibilities are configuration merge, preset-policy and rewrite-level resolution, conservative response eligibility, SHA-256 content and variant keys, and final acceptance or rejection of transform candidates.

Presets and rewrite levels are mutually exclusive selectors at one scope. A child may replace the inherited selector type. Passthrough resolves to an empty policy and stops before response inspection; the remaining levels select tested family and safety masks, while filter execution remains independently gated by its implementation evidence.

The candidate gate preserves a borrowed view of the original and selects optimized output only when the producer reports it valid and it is strictly smaller and non-identical. The caller owns both buffers and must keep their storage alive while the result is used. Equal, larger, invalid, or failed candidates resolve to the original. Variant keys use version 4 of the canonical original-content and resolved-policy encoding, so the same input and policy produce the same fleet-safe key.

## HTTP Transaction Engine

`laghu-http` defines ABI version 1 of the bounded HTTP transaction contract. Its input contains normalized method, scheme, authority, path, request headers, response status and headers, an optional strong source validator, configuration, cache path, worker queue path, and time without NGINX, APR, socket, TLS, or event-loop types. The engine opens and refreshes the queue, validates its heartbeat, and derives backend capabilities from the queue header. Header counts, names, values, captured bodies, and output operations have explicit limits.

NGINX and Apache use the same prepare and finalize operations. NGINX retains chain capture, deferred headers, and pool allocation. Apache retains repeated brigade calls, metadata buckets, `FLUSH`, `EOS`, and APR pool allocation. Both stage the complete ordered header plan before committing it and preserve the original response when staging fails. Hash-only image and CSS routes also use the engine; fixed beacon-script delivery and bounded beacon POST ingestion remain adapter-owned.

The standalone `laghu` executable is the third adapter over the same transaction engine. Its first transport supports one configured plaintext HTTP origin, one request per downstream connection, fixed-length request bodies up to 1 MiB, bounded response framing, and a fixed worker pool with a bounded accepted-connection queue. POSIX sockets and WinSock implement one internal transport contract. The proxy owns hop-by-hop header removal, deadlines, disconnect handling, and origin errors; it never retries a request. Captured responses are complete before engine output is committed, while malformed or truncated captures return a transport error without partial optimization headers.

The proxy reserves `/.laghu/` locally. Valid image and CSS hashes use the transaction engine's immutable cache path, and the fixed image beacon script and bounded same-origin POST remain adapter-owned. TLS, persistent connections, origin pooling, HTTP/2, HTTP/3, trusted forwarded headers, health endpoints, and graceful drain are not part of the bounded HTTP/1.1 transport.

The engine has prepare and finalize phases. Prepare performs response classification, policy resolution, capture planning, variant identity, and immediate warm image lookup. Finalize accepts one complete captured body, invokes the shared HTML, CSS, or image runtime, and returns the selected body, dependency and cache keys, queue-publication state, and an ordered atomic header-operation list. Inputs are borrowed; engine-owned bodies and header values have one explicit result-release function.

The engine never mutates server headers or buffers. Transport integrations remain responsible for capturing bytes and applying a complete result. Any invalid ABI, exceeded bound, incomplete body, corrupt cache entry, unavailable backend, full queue, allocation failure, or transform failure retains the original body without partial transformation headers. The conformance suite exercises the engine without linking NGINX or APR.

## libvips Service and Cache

The `laghu-libvips` service performs image transforms asynchronously through the libvips C API. It probes explicit codec operations at startup and publishes the capability mask in the queue header; neither server adapter links libvips nor shells out. For Apache static files, the source modification time and size provide the stable source validator when Apache weakens an origin ETag after inserting an output filter. Proxied responses still require a strong origin validator for an immediate warm lookup. A first cache miss serves the original response; a successful optimization writes a content-addressed variant for later requests. Worker absence, timeouts, parse errors, and codec failures all preserve the original.

Image variant key version 4 includes source bytes, resolved policy and quality, service build/libvips identity, frozen encoder options, capability mask, complete transform flags, geometry, lossy permission, and browser-format acceptance. Temp-file, fsync, and rename publication prevents readers from observing partial variants; a separate payload SHA-256 rejects same-length corruption before headers change. The queue heartbeat makes a stopped worker unavailable after the liveness window.

## Response Path

Each module asks `laghu-core` whether a response is eligible. It streams the cold original while copying a bounded image into a queue slot. A warm strong-validator hit is read before response headers are committed and replaces the origin body. Queue publication uses an immediate-fail platform lock, so contention cannot hold a server request. POSIX and Win32 runtime backends expose the same queue/cache contract. Queue protocol v5 supports image jobs carrying one bounded source payload and sprite jobs carrying only validated variant keys.

External stylesheets use the same lifecycle. A bounded server-independent CSS tokenizer discovers same-origin image dependencies without fetching them. The first eligible response remains byte-identical; after every dependency and any sprite are atomically published, a later request may receive derived CSS with a dependency ETag. Both adapters preserve the original on parse, queue, cache, or allocation failure.

The shared HTML planner can combine remaining adjacent stylesheet records after small-sheet inlining. It reads only finalized catalog payloads, validates nested dependencies and CSP, concatenates in DOM order, reparses the bounded bundle, and atomically publishes one immutable asset. Publication and HTML derivation are separate cold stages; only a later warm request receives the link, and only when the full HTML plus unique-CSS transfer decreases.

Before catalog-driven CSS transformations, the same server-independent planner runs a bounded document-structure pass. It conservatively adds a missing head, merges only whitespace/comment-adjacent heads, and moves eligible stylesheet links and complete style blocks into that head while retaining source order and original node bytes. Script boundaries are policy inputs: only policies that explicitly permit script reordering may place CSS ahead of an executable classic or module script. NGINX and Apache pass identical planner flags and use the same cold-original, dependency-key, byte-accounting, and fail-open path. The flags form a versioned planner mask rather than adapter-specific booleans. When HTML minification is selected, the planner removes eligible comments and exact default MIME attributes, safely unquotes values, and collapses ordinary ASCII text whitespace. Raw-text, template/noscript, SVG/MathML, legacy raw-text, and content-editable regions remain opaque. The derived document is reparsed for ordered structural equivalence, and lexical changes are accepted only when strictly smaller.

## Safe Metadata and Resource Hints

The versioned HTML planner converts only a complete, head-level `Content-Language` HTTP-equivalent meta element. An absent response header is added on the warm response; an identical origin header is retained. Conflicting headers, conflicting meta elements, malformed values, duplicate attributes, and every security-sensitive or legacy `http-equiv` value preserve the original document and headers.

Policies selecting resource hints may emit up to four preload and eight DNS-prefetch HTTP `Link` values. Preloads are restricted to validated, ready same-origin stylesheet records and ready first, high-priority, or learned above-fold image variants. DNS-prefetch discovers only third-party HTTP(S) origins already referenced by bounded HTML parsing. Existing response and HTML hints are deduplicated. Discovery never resolves a hostname, fetches a resource, or creates a catalog record.

The cold response contains neither converted metadata nor new hint headers. After the content-addressed derivation is validated, both adapters apply the body and all header operations together, remove stale entity digests when the body changes, and emit a dependency-derived strong ETag. Allocation, parsing, catalog, or header validation failures remain fail-open.
