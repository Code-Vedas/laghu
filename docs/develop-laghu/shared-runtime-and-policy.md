---
title: Shared runtime and policy
parent: Develop Laghu
---
# Shared runtime and policy

The shared runtime owns policy, cache, queue, worker, HTTP, and fail-open behavior. Adapters translate native server events into that contract; do not duplicate policy in an adapter.
