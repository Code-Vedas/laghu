---
applyTo: "**"
---

# Laghu Review Instructions

Prioritize correctness, event-loop safety, resource bounds, and documentation
truth over cosmetic feedback.

## Required Traces

- Follow every module failure path to the next NGINX filter or original-body
  delivery. Flag any path that can drop, duplicate, or indefinitely retain a
  chain.
- Identify exact allocation ownership and lifetime for request, configuration,
  shared-memory, and worker data.
- Trace directive tokens from raw NGINX input through validation, inheritance,
  and core policy. Validation must occur before normalization.
- For asynchronous work, trace cancellation, timeout, worker loss, cache
  publication, and duplicate first-hit behavior.
- Check that response eligibility respects authentication and cache-control
  boundaries before any buffering or transform begins.

## Public Surface

- Verify that README, docs, examples, scripts, and workflows use the same paths
  and commands.
- Flag shipped-capability claims that are only roadmap items.
- Require direct tests for new directive values, policy branches, and fail-open
  behavior.
- Treat performance claims as unproven until the relevant workload, latency,
  CPU, memory, size, and correctness evidence exists.

