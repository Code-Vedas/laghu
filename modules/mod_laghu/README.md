# mod_laghu

`mod_laghu` is Laghu's Apache HTTP Server 2.4 output-filter adapter. It uses
the same policy, queue, cache, and `laghu-libvips` service as `ngx-laghu`; the
module never links libvips or runs codec commands inside an Apache process.

Build with `scripts/build-apache-module`, load `mod_laghu.so`, and configure it
with native `Laghu` directives. Package installation loads the module with
`Laghu Off` so operators explicitly choose when optimization begins.

Image markup settings mirror NGINX: `Laghu ImageBeacon On|Off`,
`Laghu ImageInlineLimit 0..16384`, `Laghu ImageMetadataLimit 1..100000`, and
`Laghu ImageMetadataTtl 1h..30d`.
