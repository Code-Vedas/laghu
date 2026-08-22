# laghu-libvips

This worker performs isolated image transformations for Laghu. NGINX, Apache, and standalone publish jobs to its queue; no web-server process invokes codecs.

Initialize and serve a queue with `laghu-libvips --init <queue> <cache>` followed by `laghu-libvips --serve <queue> <cache>`.
