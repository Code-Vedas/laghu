# Laghu Image

This library provides server-independent image probing, transforms, validation, and markup decisions. It is used by all delivery surfaces and by `laghu-libvips`.

Configure with `-DLAGHU_WITH_VIPS=ON` for libvips transforms, then build and run `ctest --test-dir build -R laghu_image_test`.
