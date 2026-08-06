# `laghu-asset-upload`

`laghu-asset-upload` is Laghu's optional asynchronous CDN publication worker. Request adapters publish bounded local jobs and continue serving the origin response; the worker uploads or verifies immutable objects and atomically marks catalog records ready.

Run one queued job with `laghu-asset-upload --once /etc/laghu/asset-offload.conf`, or run continuously with `--serve`. The configuration names environment variables containing the S3 access and secret keys; it never contains credential values.

Origin fallback follows at most three redirects, and only root-relative or exact mapped-origin HTTPS locations are accepted.

The worker uses verified HTTPS, AWS Signature Version 4, content SHA-256 metadata, bounded retries, and fail-open catalog states. A failed provider request cannot change the origin response.
