# mod_laghu

`mod_laghu` is Laghu's Apache HTTP Server 2.4 output filter. It uses the same policy, queue, cache, and `laghu-libvips` service as `ngx-laghu`; the module never links libvips or runs codec commands inside an Apache process.

Build with `scripts/build-apache-module`, load `mod_laghu.so`, and configure it with native `Laghu` directives. Package installation loads the module with `Laghu Off` so operators explicitly choose when optimization begins.

Image markup settings mirror NGINX: `Laghu ImageBeacon On|Off`, `Laghu ImageInlineLimit 0..16384`, `Laghu ImageMetadataLimit 1..100000`, and `Laghu ImageMetadataTtl 1h..30d`.

CSS markup settings also mirror NGINX: `Laghu CssInlineLimit 0..65536` (default 2048) and `Laghu CssOutlineThreshold 1024..1048576` (default 8192). Small catalog-ready stylesheets may be inlined and eligible large style blocks may be outlined to immutable `/.laghu/css/<sha256>` assets; both paths preserve the cold response and fail open.
