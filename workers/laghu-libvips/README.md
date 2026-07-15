# Laghu Optimizer

`laghu-libvips` consumes bounded image jobs outside NGINX. It probes libvips,
forks an isolated child for each conversion, enforces a 30-second deadline, and
publishes only validated, strictly smaller variants.

Use `--init <queue> <cache>` once before `--serve <queue> <cache>`. `--probe`
prints the effective backend identity and codec capability mask and fails when
no image operation is available. Partial custom backends warn once with the
missing operations. `--transform` is a diagnostic path; NGINX communicates
only through the queue and never executes it. Single-process containers can use
`--init-and-serve`; it execs a clean service process before any job forks.
