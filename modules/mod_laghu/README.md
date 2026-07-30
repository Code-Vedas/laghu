# mod_laghu

`mod_laghu` is Laghu's Apache HTTP Server 2.4 output filter. It normalizes APR tables for `laghu-http` without collapsing duplicate headers, supplies the static-file modification-time and size validator when required, and copies shared results into the request pool. Repeated brigade calls, metadata buckets, `FLUSH`, and `EOS` remain native to the module. It uses the same queue, cache, and `laghu-libvips` service as `ngx-laghu`; the module never links libvips or runs codec commands inside an Apache process.

Build with `scripts/build-apache-module`, load `mod_laghu.so`, and configure it with native `Laghu` directives. Package installation loads the module with `Laghu Off` so operators explicitly choose when optimization begins.

Image markup settings mirror NGINX: `Laghu ImageBeacon On|Off`, `Laghu ImageInlineLimit 0..16384`, `Laghu ImageMetadataLimit 1..100000`, and `Laghu ImageMetadataTtl 1h..30d`.

Critical-CSS learning is separately opt-in through `Laghu CriticalCssBeacon On|Off` and uses the same bounded, privacy-preserving shared runtime as NGINX and standalone Laghu.

CSS markup settings also mirror NGINX: `Laghu CssInlineLimit 0..65536` (default 2048) and `Laghu CssOutlineThreshold 1024..1048576` (default 8192). Small catalog-ready stylesheets may be inlined and eligible large style blocks may be outlined to immutable `/.laghu/css/<sha256>` assets; both paths preserve the cold response and fail open.

JavaScript markup settings are independent: `Laghu JavaScriptInlineLimit 0..65536` defaults to 2048 and `Laghu JavaScriptOutlineThreshold 1024..1048576` defaults to 8192. Ready URL-independent scripts may be inlined or outlined, while 2 through 16 adjacent classic external scripts can be combined only through a shared `data-laghu-combine` marker and SWC safety certificate. All outputs remain subject to the aggregate transfer-size gate.
