# Laghu Image

`laghu-image` is the server-independent image pipeline. With
`LAGHU_WITH_VIPS=ON` it uses only explicit JPEG, PNG, GIF, and WebP libvips C
load/save operations. The `OFF` build retains format, key, and markup APIs but
always preserves original image bytes.

Every byte candidate is decoded and structurally validated before the core
strictly-smaller gate can select it. Lossless candidates additionally require
normalized pixel identity; animation validates frames, delays, loop count, and
alpha. Markup helpers operate only on a caller-provided ready-variant catalog.
