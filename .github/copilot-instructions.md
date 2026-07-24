# Laghu Repository Instructions

## Placement

- `libs/laghu-core` owns server-independent policy and direct C tests.
- `libs/laghu-image` owns explicit-loader image transforms and image-markup primitives; libvips remains private to this library and its worker consumer.
- `libs/laghu-runtime` owns the server-independent queue/cache protocol.
- `workers/laghu-libvips` owns codec probing, isolated execution, and atomic publication. Server adapters must never link codec libraries or invoke codec commands.
- `modules/ngx_http_laghu_module` owns NGINX types, directives, lifecycle, and response-filter integration.
- `modules/mod_laghu` owns Apache types, directives, lifecycle, APR pools, and bucket-brigade integration.
- `ngx-laghu` and `mod-laghu` are equal product surfaces. Shared filters belong in libraries; never implement a filter only inside one module.
- `docs/` owns supported public product behavior.
- `scripts/` owns repository-wide local and CI entrypoints.

## Planning and Progress Tracking

- `1-pager.md` is the private north-star product specification. Keep it timeless: no milestones, completion status, delivery phases, or incremental scheduling.
- `ROADMAP.md` is a temporary internal execution tracker, not public product documentation. Keep the complete work inventory there until the tracker is intentionally retired.
- `ROADMAP.md` is the only Markdown file allowed to describe pending work, milestones, sequencing, or incomplete validation. Every other Markdown file is production documentation and must state implemented behavior only.
- Use `[x]` only for implemented work backed by executable validation. Leave incomplete or partially scaffolded work as `[ ]`.
- Never delete an unchecked item to make progress appear complete. Add new scope to the tracker before implementing it and return invalidated claims to `[ ]`.
- Temporary private planning inputs are reference material only. Never link, cite, import, or mention them in README files, public documentation, code, tests, comments, examples, generated documentation, or agent reports.
- Public documentation and code must remain correct after all temporary planning and tracking files are deleted.
- Write one source line per prose paragraph or list item. Do not hard-wrap Markdown at an arbitrary column; preserve blank lines only where Markdown structure requires them.

## Runtime Rules

- Preserve the original response on every service failure.
- Never perform blocking fetch, codec, or service waits in a server request.
- Keep NGINX allocation lifetimes explicit and use request/configuration pools.
- Keep Apache allocation lifetimes explicit and preserve brigade metadata, `FLUSH`, and `EOS` behavior.
- Validate raw directive input before applying defaults or inheritance.
- Bound content size, memory, time, cache growth, and variant fanout.
- Do not add outbound fetching without SSRF-safe defaults and threat modeling.

## Product Truth

- Do not present a roadmap filter, endpoint, CLI command, package, or server target as available until executable validation exists.
- Keep public runtime namespaces under `laghu`; historical names may appear only in the dedicated background page and internal progress tracker.
- Update README, docs, scripts, and CI together when ownership or commands move.
- Expose `ngx-laghu` and `mod-laghu` as user-facing package identities. `laghu-libvips` is their internal shared dependency.

## License Headers

- Every source file and every comment-capable script, build definition, workflow, template, style, and deployment or packaging configuration must carry the repository's full Codevedas MIT license header.
- Keep the shebang, Ruby magic comment, or Jekyll front matter first when the file format requires it. Prose, legal texts, generated lockfiles, CNAME, and strict package-format metadata do not receive a synthetic comment header.
- Run `scripts/run-license-headers-all`; `scripts/run-lint-all` and `scripts/run-all` enforce the same audit.

## Verification

Run `scripts/run-all` for the full repository lane. Use the focused scripts under `scripts/` while iterating.
