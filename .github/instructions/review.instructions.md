---
applyTo: "**"
---

# Laghu Review Instructions

Prioritize correctness, event-loop safety, resource bounds, and documentation truth over cosmetic feedback.

## Required Traces

- Follow every adapter failure path to original-body delivery. For NGINX, flag chains that can be dropped, duplicated, or indefinitely retained. For Apache, include brigades, metadata, `FLUSH`, and `EOS`.
- Identify exact allocation ownership and lifetime for request, configuration, shared-memory, and worker data.
- Trace configuration tokens from each native surface through validation, inheritance or scope resolution, and core policy. Validation must occur before normalization.
- For asynchronous work, trace cancellation, timeout, worker loss, cache publication, and duplicate first-hit behavior.
- Check that response eligibility respects authentication and cache-control boundaries before any buffering or transform begins.

## Public Surface

- Verify that README, docs, examples, scripts, and workflows use the same paths and commands.
- Flag shipped-capability claims that are only roadmap items.
- Require pending work, milestones, sequencing, and incomplete validation to remain exclusively in `ROADMAP.md`; keep `1-pager.md` timeless and every other Markdown file production-ready.
- Reject arbitrary hard wrapping in Markdown prose. Each paragraph and list item uses one source line unless Markdown syntax requires another layout.
- Require direct tests for new directive values, policy branches, and fail-open behavior.
- Reject filter logic implemented privately in one module. Shared behavior must use common libraries and direct tests through both adapters.
- Treat performance claims as unproven until the relevant workload, latency, CPU, memory, size, and correctness evidence exists.
