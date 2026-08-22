# SyncChat Step 10 Stress/Concurrency Tests

Run these tests only against your own local SyncChat server.

Examples:

```bash
python3 tests/stress_syncchat.py chat --clients 10 --messages 20
python3 tests/stress_syncchat.py chat --clients 25 --messages 20
python3 tests/stress_syncchat.py chat --clients 50 --messages 10

python3 tests/stress_syncchat.py download \
    --clients 5 \
    --file perf-test.bin \
    --expected /tmp/perf-test.bin

python3 tests/stress_syncchat.py limit --limit 64
```

The chat test verifies every expected broadcast exactly once.

The download test verifies that concurrent readers receive identical
byte counts and SHA-256 hashes.

The limit test fills the configured client registry and expects the
next login to be rejected with SERVER_CLIENT_LIMIT.
