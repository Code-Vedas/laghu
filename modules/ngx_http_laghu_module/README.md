# ngx_http_laghu_module

This directory contains the thin NGINX integration layer. It normalizes native HTTP metadata for `laghu-http`, captures only the body class and bound selected by the shared engine, copies selected output into the request pool, and applies the engine's ordered header plan. NGINX chain handling and deferred headers remain native to the module.

It:

- builds as a static or dynamic HTTP auxiliary filter module
- accepts `laghu on|off`, `laghu preset <name>`, `laghu rewrite_level <name>`, `laghu allow_api on|off`, `laghu image_quality 1..100`, `laghu image_beacon on|off`, `laghu image_inline_limit 0..16384`, `laghu image_metadata_limit 1..100000`, `laghu image_metadata_ttl 1h..30d`, `laghu css_inline_limit 0..65536`, `laghu css_outline_threshold 1024..1048576`, `laghu worker_queue <path>`, and `laghu image_cache <path>` at `http`, `server`, and `location` scope
- inherits configuration through NGINX's normal location hierarchy
- delegates response eligibility, policy and backend resolution, cache lookup, transformation orchestration, body selection, and header planning to `laghu-http`
- serves an already-published image variant on a strong-validator cache hit
- captures bounded cold image responses while streaming the original and uses a try-only shared queue publication
- never links libvips or invokes a codec command

Build it against an NGINX source tree with:

```bash
./configure --with-compat \
  --add-dynamic-module=/path/to/laghu/modules/ngx_http_laghu_module
make modules
```

The generated artifact is `objs/ngx_http_laghu_module.so`.
