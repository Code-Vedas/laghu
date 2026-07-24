# Laghu Core

`laghu-core` owns optimization policy that does not depend on NGINX. It includes configuration inheritance, preset and rewrite-level policy resolution, conservative response eligibility, dependency-free SHA-256 content and variant keys, and a candidate gate that preserves a borrowed view of the caller-owned original unless validated output is strictly smaller. Version 4 keys include the resolved image quality used by the asynchronous image pipeline.

Preset and rewrite-level selectors are mutually exclusive within one scope. Rewrite levels provide passthrough, core, bandwidth, all, and experimental policy masks.

The public C API lives in `include/laghu/core.h`. Keeping it independent from server types lets NGINX, Apache, and the workers share policy without linking transport internals into the engine.
