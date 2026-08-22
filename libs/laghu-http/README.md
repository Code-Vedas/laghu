# laghu-http

This library turns normalized request and response data into a bounded transformation plan. Native adapters own transport, allocation, cancellation, and header application; this library owns eligibility, cache selection, transformation sequencing, and fail-open results.

NGINX, Apache, and standalone use `laghu_http_transaction_prepare` and `laghu_http_transaction_finalize`. Build from the repository root and run `ctest --test-dir build -R laghu_http_test`.
