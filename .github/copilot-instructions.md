# Laghu Repository Instructions

## Priority

- Minimize context/output: search/filter first; read only relevant ranges; never dump large files, logs, diffs, JSONL, schemas, docs, config, or tool metadata unless required.
- Reuse known context; do not reread unchanged content, restate history, repeat output, or carry completed investigation forward.
- Ignore generated/vendor/lock/build/cache/coverage/minified content unless relevant.
- Keep updates/reports terse; show output only as evidence.

## Ownership

- `laghu-base`: bounded dependency-free primitives.
- `laghu-markup`: bounded tokenization.
- `laghu-core`: config/policy, eligibility, selection, C tests.
- `laghu-image`: image transforms/probes/markup; libvips private.
- `laghu-runtime`: caches, queues, catalogs, RUM, state, transformation services.
- `laghu-http`: transactions, sequencing, caching, headers, publication, budgets, fail-open results.
- `laghu-libvips`: isolated codec probing/execution/publication; server modules never link/invoke codecs.
- NGINX/Apache modules contain server-specific integration only; shared filters belong in libraries and both servers remain equal product surfaces.
- `docs/` = public behavior; `scripts/` = repo/CI entrypoints; `cmake/laghu-sources.list` = canonical native sources.

## Truth

- `1-pager.md`: timeless private product specification.
- `ROADMAP.md`: only Markdown allowed to contain pending work, milestones, sequencing, or incomplete validation.
- `[x]` requires executable validation; never hide unfinished or invalidated work.
- Temporary planning material must never leak into public artifacts.
- Public docs describe implemented behavior only and must stay synchronized with code/scripts/CI.
- Public namespace is `laghu`; packages are `ngx-laghu` and `mod-laghu`; `laghu-libvips` is internal.
- Markdown: one source line per paragraph/list item; no hard wrapping.

## Runtime

- Fail open; preserve original responses on service failure.
- Never block requests on fetches, codecs, or services.
- Respect NGINX pool and Apache APR/brigade lifetimes, metadata, `FLUSH`, and `EOS`.
- Validate raw directives before inheritance/defaults.
- Bound content, memory, time, cache growth, and variant fanout.
- Outbound fetching requires SSRF-safe defaults and threat modeling.

## License

- All comment-capable source/config/build/workflow/deployment files require the full Codevedas MIT header.
- Preserve required shebang/magic/front matter first; exclude prose, legal text, lockfiles, CNAME, and strict package metadata.
- License checks are enforced by `run-license-headers-all`, `run-lint-all`, and `run-all`.

## Verification

- Iterate with focused targets; run `scripts/run-all` after they pass or shared contracts require it.
- Log verbose build/install/package output; report only phase results and bounded actionable failures.
- Reuse unchanged artifacts and retry only failed phases.
- Prefer bounded polling and logged remote jobs.
- Transfer only tracked + intentional source changes; exclude generated/build/dependency trees and `.git/`.
- Final reports: commands/phases, outcomes, unresolved failures—no routine logs.
