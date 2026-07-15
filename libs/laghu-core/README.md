# Laghu Core

`laghu-core` owns optimization policy that does not depend on NGINX. It includes
configuration inheritance, preset-policy resolution, conservative response
eligibility, dependency-free SHA-256 content and variant keys, and a candidate
gate that preserves a borrowed view of the caller-owned original unless
validated output is strictly smaller. Transformation engines, cache storage,
and the worker protocol will be added behind this boundary.

The public C API lives in `include/laghu/core.h`. Keeping it independent from
NGINX lets the future worker, sidecar, CLI, and module share policy without
linking server internals into the optimization engine.
