# Laghu Optimizer

`laghu-libvips` consumes bounded image jobs outside NGINX and Apache. It probes libvips, forks an isolated child for each conversion, enforces a 30-second deadline, and publishes only validated, strictly smaller variants. Large bounded animated GIFs additionally use the packaged FFmpeg executable in that same isolated worker to publish MP4 and WebM artifacts; absent or failed video conversion preserves the GIF. It is an internal transform service rather than a user-facing package.

Use `--init <queue> <cache>` once before `--serve <queue> <cache>`. `--probe` prints the effective backend identity and codec capability mask and fails when no image operation is available. Partial custom backends warn once with the missing operations. `--transform` is a diagnostic path; the server modules communicate only through the queue and never execute it. Single-process containers can use `--init-and-serve`; it execs a clean service process before any job forks.
