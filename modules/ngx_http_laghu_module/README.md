# ngx_http_laghu_module

This directory contains the thin NGINX integration layer. The skeleton:

- builds as a static or dynamic HTTP auxiliary filter module
- accepts `laghu on|off`, `laghu preset <name>`, and
  `laghu allow_api on|off` at `http`, `server`, and `location` scope
- inherits configuration through NGINX's normal location hierarchy
- delegates preset resolution and response eligibility, including default API
  path exclusion, to `laghu-core`
- emits `X-Laghu` with the pass/bypass decision while leaving the body untouched
- installs a fail-open body-filter seam for future optimization work

Build it against an NGINX source tree with:

```bash
./configure --with-compat \
  --add-dynamic-module=/path/to/laghu/modules/ngx_http_laghu_module
make modules
```

The generated artifact is `objs/ngx_http_laghu_module.so`.
