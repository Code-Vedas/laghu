# Local AMD64 performance rail

Run only on an AMD64 Linux host. This rail is intentionally local-only: it is never called by CI.

`scripts/run-benchmarks-all` builds a deterministic corpus, starts plain NGINX, frozen ngx_pagespeed 1.13.35.2, and current Laghu/NGINX, measures cold, warm, and capability requests, then writes JSON and HTML artifacts under `tmp/benchmarks/`. Capability results retain each HTML-emitted image URL and its response type, so PageSpeed rewrite output and Laghu transform state are compared as observed.

The report records any unavailable optional measurement as `unavailable`; it never substitutes an estimate for image quality or CWV data.
