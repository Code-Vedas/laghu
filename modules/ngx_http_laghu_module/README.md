# ngx_http_laghu_module

This directory contains the thin NGINX integration layer. It:

- builds as a static or dynamic HTTP auxiliary filter module
- accepts `laghu on|off`, `laghu preset <name>`,
  `laghu rewrite_level <name>`, `laghu allow_api on|off`,
  `laghu image_quality 1..100`, `laghu worker_queue <path>`, and
  `laghu image_cache <path>` at `http`, `server`, and `location` scope
- inherits configuration through NGINX's normal location hierarchy
- delegates preset/rewrite-level resolution and response eligibility, including
  intentional passthrough and default API exclusion, to `laghu-core`
- serves an already-published image variant on a strong-validator cache hit
- captures bounded cold image responses while streaming the original and uses
  a try-only shared queue publication
- never links libvips or invokes a codec command

Build it against an NGINX source tree with:

```bash
./configure --with-compat \
  --add-dynamic-module=/path/to/laghu/modules/ngx_http_laghu_module
make modules
```

The generated artifact is `objs/ngx_http_laghu_module.so`.
