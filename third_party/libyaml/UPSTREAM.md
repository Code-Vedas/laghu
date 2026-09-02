# libyaml source

Laghu vendors the parser-only subset of [libyaml 0.2.5](https://github.com/yaml/libyaml).

- upstream tag: `0.2.5`
- upstream commit: `65d19898a301b817261003b00d1dcef00895a7b4`
- license: MIT; see [LICENSE](LICENSE)
- retained sources: parser, scanner, reader, loader, API, and their required emitter/writer support

The standalone runtime uses libyaml only to parse the strict, versioned YAML configuration. It never emits YAML or accepts arbitrary tags as configuration semantics.

Security backport: the current canonical source keeps the `0.2.6-rc.1`
1,000-level default introduced by commit
[`51843fe48257c6b7b6e70cdec1db634f64a40818`](https://github.com/yaml/libyaml/commit/51843fe48257c6b7b6e70cdec1db634f64a40818),
then completes combined block/flow scanner enforcement in commit
[`849d0aefecb42fe70165ed1557c329aff9e74d3e`](https://github.com/yaml/libyaml/commit/849d0aefecb42fe70165ed1557c329aff9e74d3e).
Laghu retains 0.2.5 and backports only that API and scanner limit; no
unreleased version identity or unrelated upstream change is claimed.
