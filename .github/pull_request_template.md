# Pull Request

## Description

Describe the behavior and why it belongs in Laghu.

## Related Issue

Link the issue or design discussion, if applicable.

## Changes

- [List the important implementation and documentation changes.]

## Safety and Performance

- Does the original response remain available on every failure path?
- Could this work block an NGINX event loop?
- What input, memory, time, or cache bounds apply?
- What performance evidence is available or still required?

## Validation

- [ ] Core build and tests pass.
- [ ] The NGINX and Apache adapters build for affected compatibility targets.
- [ ] Documentation was updated for public behavior or support changes.
- [ ] No unimplemented capability is presented as shipped.

<!-- Pull request titles should use feat:, bugfix:, docs:, chore:, refactor:, test:, ci:, perf:, build:, or release:. -->
