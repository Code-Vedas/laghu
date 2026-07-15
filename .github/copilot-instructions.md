# Laghu Repository Instructions

## Placement

- `libs/laghu-core` owns server-independent policy and direct C tests.
- `modules/ngx_http_laghu_module` owns NGINX types, directives, lifecycle, and
  response-filter integration.
- `docs/` owns supported public product behavior.
- `scripts/` owns repository-wide local and CI entrypoints.

## Planning and Progress Tracking

- `ROADMAP.md` is a temporary internal execution tracker, not public product
  documentation. Keep the complete work inventory there until the tracker is
  intentionally retired.
- Use `[x]` only for implemented work backed by executable validation. Leave
  incomplete or partially scaffolded work as `[ ]`.
- Never delete an unchecked item to make progress appear complete. Add new scope
  to the tracker before implementing it and return invalidated claims to `[ ]`.
- Temporary private planning inputs are reference material only. Never link,
  cite, import, or mention them in README files, public documentation, code,
  tests, comments, examples, generated documentation, or agent reports.
- Public documentation and code must remain correct after all temporary
  planning and tracking files are deleted.

## Runtime Rules

- Preserve the original response on every optimizer failure.
- Never perform blocking fetch, codec, or worker waits in an NGINX event loop.
- Keep NGINX allocation lifetimes explicit and use request/configuration pools.
- Validate raw directive input before applying defaults or inheritance.
- Bound content size, memory, time, cache growth, and variant fanout.
- Do not add outbound fetching without SSRF-safe defaults and threat modeling.

## Product Truth

- Do not present a roadmap filter, endpoint, CLI command, package, or server
  target as available until executable validation exists.
- Keep public runtime namespaces under `laghu`; historical names may appear only
  in the dedicated background page, the internal progress tracker, and a future
  one-way migration parser.
- Update README, docs, scripts, and CI together when ownership or commands move.

## Verification

Run `scripts/run-all` for the full repository lane. Use the focused scripts
under `scripts/` while iterating.
