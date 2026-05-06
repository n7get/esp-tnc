# Changes

## 05/05/2026

- Updated idf_component.yml to use Github instead of local filesystem.
- Broadcast local transmissions to AGWPE monitor clients.

## 04/18/2026

### BBS

- Increased maximum message body size (default 4096 bytes, configurable up to 10240).
- BBS now reports available message size and free storage space when composing a message.
- BBS gives clearer feedback when a message body is too large, and gracefully truncates rather than discarding.

### Tests

- Added `test_message` automated BBS integration test: posts messages, reads them back to verify content, deletes them, and tests oversized-message handling.
