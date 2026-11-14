Run `python tests/tuya_light/run_tests.py` from the repository root.

The harness compiles the actual light implementation, the installed SDK's
mbedTLS SHA256/MD/HMAC/platform sources and its cJSON parser. Python's independent
standard-library HMAC provides fixed token and command signature vectors. All
credentials, hosts, device IDs and responses are synthetic; no network or UART
is used. The source never logs request headers, bodies, credentials or tokens.

Coverage includes all four boot masks and sixteen transitions, duplicate and
stale sequences, sequence/timer wrap, coalescing, expiry without age refresh,
changes during token HTTPS, invalid token responses, synchronized 64-bit UTC,
token cache/skew, bounded expired-token retry across foreground deferrals, and
unknown delivery without blind retry. The worker-facing HTTP fake checks that
no light mutex crosses HTTP and every request has an explicit CA and limits.
The light state is at most 384 bytes on the host; it creates no thread or queue.

Initial missing-feature RED: `red-result.txt`. A further actual-source RED for
retry-budget reset across a deferred retry: `deferred-retry-red-result.txt`.
Final actual-source result: `green-result.txt`. `--red` uses inert public APIs
to check test sensitivity. These tests do not establish live CA trust, account
permissions, lamp command acceptance or acoustic behavior of the merged image.
