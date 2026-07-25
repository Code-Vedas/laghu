# laghu-http

`laghu-http` is the server-neutral HTTP transaction engine shared by Laghu deployment adapters. It owns response eligibility, policy resolution, cache lookup, transformation orchestration, dependency identity, header planning, body selection, and fail-open behavior.

Transport adapters retain configuration parsing, HTTP framing, response capture, server-owned allocation, header application, cancellation, timeouts, and backpressure. NGINX chains and deferred headers, Apache bucket metadata including `FLUSH` and `EOS`, and transport sockets remain outside this library.

The API has two phases. `laghu_http_transaction_prepare` decides whether a response bypasses processing, requires bounded capture, or can use a warm cached body. `laghu_http_transaction_finalize` accepts a complete captured body and returns an atomic body-and-header plan. Inputs are borrowed; result-owned storage is released with `laghu_http_transaction_result_release`.

ABI version 1 accepts an optional explicit strong source validator for server-native validators that are not HTTP headers. It opens and refreshes the configured worker queue, rejects missing or stale heartbeats, derives codec capabilities from the queue header, parses image client hints, and carries catalog-selected target geometry into image jobs.

`ngx-laghu` and `mod-laghu` normalize native request and response metadata into this contract. They copy selected bodies and header values into server-owned pools before releasing results. Beacon request ingestion remains native because it mutates learning metadata rather than transforming a response.
