# Validation for configuration, cache, CI, and HTTPS

- GCC strict C11 build with warnings treated as errors passed.
- TLS-enabled integration suite: 38 tests, 37 passed and one optional process-descriptor check skipped because this runtime does not expose the descriptor table.
- AddressSanitizer and UndefinedBehaviorSanitizer: same integration suite passed; the cache ownership unit test also passed with instrumentation. Leak detection was disabled locally because this runtime lacks the required process access. CI enables leak detection on Ubuntu.
- HTTP-only strict build: 38 tests, 34 passed, three HTTPS tests and the optional descriptor check skipped.
- Cache unit test covers pinned data, capacity, expiry retirement, release, and ownership.
- Deterministic parser/range fuzzing: 100,000 mutations passed.
- Verified HTTPS benchmark and stress smoke runs: 100 requests each, zero errors. These are functional checks, not throughput baselines.
- GitHub Actions workflow supplied; remote CI and Clang coverage-guided fuzzing have not been run in this session.
