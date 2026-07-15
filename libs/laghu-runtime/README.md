# Laghu Runtime

`laghu-runtime` owns the file-backed bounded job queue, worker capability
publication, deterministic request index, and atomic content-addressed disk
publication. It has no NGINX or codec dependency. Queue publication uses a
non-blocking file lock; failure always means “serve the original.” Consumers
can retain a mapping and refresh the worker heartbeat/capability header without
reopening the queue on every request. Cache metadata carries a payload SHA-256,
so same-length corruption is rejected before a variant is served.
