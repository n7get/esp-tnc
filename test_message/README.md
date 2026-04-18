# test_message

Automated BBS integration test for ESP-TNC.

This test application connects to a running BBS (esp-tnc) instance (on a separate device) over AX.25/KISS TCP, posts messages, reads them back, deletes them, and verifies correct protocol behavior. It also tests oversized message handling and BBS truncation/warning logic.

## What it tests
- Posting a message and verifying it is stored and listed
- Reading and deleting messages
- Posting a message that exceeds the BBS body size limit (verifies truncation and warning)
- BBS prompt and warning output
- End-to-end protocol compliance

## How to use
1. Build and flash the app to a test ESP32 device.
2. Configure WiFi and BBS connection parameters in menuconfig.
3. Start the BBS (esp-tnc) on another device.
4. Run the test; results and pass/fail summary are printed to the log.
