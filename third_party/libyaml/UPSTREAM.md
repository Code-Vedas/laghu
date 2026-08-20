# libyaml source

Laghu vendors the parser-only subset of [libyaml 0.2.5](https://github.com/yaml/libyaml).

- upstream tag: `0.2.5`
- upstream commit: `65d19898a301b817261003b00d1dcef00895a7b4`
- license: MIT; see [LICENSE](LICENSE)
- retained sources: parser, scanner, reader, loader, API, and their required emitter/writer support

The standalone runtime uses libyaml only to parse the strict, versioned YAML configuration. It never emits YAML or accepts arbitrary tags as configuration semantics.
