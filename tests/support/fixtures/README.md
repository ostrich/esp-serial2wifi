# Synthetic API fixtures

These responses contain dummy identities/settings and accompany the versioned
API tests. Never put real passwords, device flash or private settings here.
The loopback-only `tests/support/fixture_server.py` serves them and refuses device writes.
Its scenario endpoint supplies bounded delay, HTTP errors, malformed JSON and
disconnects. Use the browser/test commands in [building](../../../docs/development.md).
