# Laghu Review Instructions

Prioritize correctness, event-loop safety, bounded resources, and documentation truth.

## Review

- Trace adapter failures to original-body delivery; catch NGINX chain loss/duplication/retention and Apache brigade/metadata/`FLUSH`/`EOS` errors.
- Verify ownership/lifetime of request, config, shared-memory, and worker allocations.
- Trace configuration from native input through pre-normalization validation, scope/inheritance, and core policy.
- For async paths, verify cancellation, timeout, worker loss, publication, and duplicate first-hit behavior.
- Enforce auth/cache-control eligibility before buffering or transformation.
- Shared filters belong in common libraries and require direct tests through both adapters.
- Require tests for new directive values, policy branches, and fail-open behavior.

## Product Truth

- README/docs/examples/scripts/workflows must agree on paths and commands.
- Public docs describe shipped behavior only; roadmap claims stay in `ROADMAP.md`, while `1-pager.md` remains timeless.
- Markdown uses one source line per paragraph/list item; no arbitrary hard wrapping.
- Treat performance claims as unproven without workload, latency, CPU, memory, size, and correctness evidence.
