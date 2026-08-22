# Laghu

Laghu is a content optimization system available as a native NGINX module, an Apache HTTP Server module, or an independent standalone server. Each surface uses the shared policy, runtime, cache, and workers.

Build and test the checkout:

```sh
cmake -S . -B build -DLAGHU_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Use the native modules from `modules/`, the standalone service from `servers/laghu/`, and the deployment material in `docs/`.

## Layout

- `libs/` shared policy, runtime, HTTP, image, and utility libraries.
- `modules/` native NGINX and Apache adapters.
- `servers/laghu/` standalone YAML-configured service.
- `workers/` isolated transformation services.
- `docs/` operator and contributor documentation.
- `packaging/` distribution and container inputs.
