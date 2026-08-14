# Local AMD64 performance rail

Run only on an AMD64 Linux host. This rail is intentionally local-only: it is never called by CI.

`scripts/run-benchmarks-all` builds a deterministic corpus, starts plain NGINX, frozen ngx_pagespeed 1.13.35.2, and current Laghu/NGINX, measures cold, warm, and capability requests, then writes JSON and HTML artifacts under `tmp/benchmarks/`. The sustained image lane follows each target's HTML-emitted URL; therefore ngx_pagespeed is measured through its rewritten image URL, not its original source URL. Before resource comparison, it requires ngx_pagespeed and Laghu to return WebP at configured quality 82 and source dimensions; the JSON contract records URL, codec, dimensions, bytes, and SSIM.

The report records any unavailable optional measurement as `unavailable`; it never substitutes an estimate for image quality or CWV data.
