# Laghu Core

`laghu-core` owns optimization policy that does not depend on NGINX. The first
scaffold includes configuration inheritance, preset parsing, and conservative
response eligibility decisions. Transformation engines, cache storage, and the
worker protocol will be added behind this boundary.

The public C API lives in `include/laghu/core.h`. Keeping it independent from
NGINX lets the future worker, sidecar, CLI, and module share policy without
linking server internals into the optimization engine.

