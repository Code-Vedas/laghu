---
title: Startup and Runtime
parent: Troubleshooting
grand_parent: Laghu Server
nav_order: 1
permalink: /laghu-server/troubleshooting/runtime/
---

# Laghu Server Startup and Runtime

## Startup Refuses Configuration

Run configuration and filesystem checks with the same environment and identity as the service.
Check required listener/origin/cache/queue settings, duplicate options, cross-option requirements, provider files, CA paths, trusted-proxy CIDRs, Redis URI expansion, and filesystem access.

## Listener Is Unreachable

Confirm the bound address/port, downstream TLS certificate and key, firewall, load-balancer target, HTTP protocol negotiation, and readiness result.
Distinguish a process that never bound from a healthy process removed by ingress health checks.

## Origin Returns 502 or 504

Inspect DNS, routing, connection-pool saturation, connect/I/O deadlines, TLS chain and hostname, SNI, origin protocol, response framing, and upstream health.
An unavailable origin cannot fail open because the proxy has no origin response to preserve.

## Graceful Shutdown Stalls

Use status metrics to identify active downstream/origin streams and compare elapsed drain time with configuration.
After the bounded deadline, the supervisor may terminate the old process; size rollout surge so replacement capacity is already ready.

| Symptom | Check | Expected recovery |
| --- | --- | --- |
| Startup exits with usage | Confirm all four required options, unique options, ranges, and cross-option requirements. | Correct arguments; invalid configuration never opens the listener. |
| Clients receive `502 Bad Gateway` | Check origin address, connect/I/O timeouts, protocol, CA trust, and origin response framing. | Restore origin connectivity or trust; the proxy does not bypass an unreachable origin. |
| Forwarded identity is ignored | Check forwarding mode and that the direct peer matches a configured CIDR. | Add only the actual trusted proxy network. |
| Responses are valid but never optimized | Check queue worker, cache permissions, eligibility, and first-hit cold behavior. | Restore asynchronous services and allow a later request to use ready output. |
| Shutdown exceeds expectations | Check active origin requests and `--drain-timeout`. | Let bounded drain complete or reduce the timeout for the next start. |
| RUM does not persist or converge | Check snapshot access, hiredis loading, `rediss://` trust, and backend durability. | Memory remains authoritative during the process lifetime; repair background persistence. |

Use the standalone smoke scripts under `scripts/` to reproduce behavior against controlled origins before changing production limits.
