# test_bbs


Automated BBS command test suite for ESP-TNC.

This app connects to a BBS (esp-tnc) instance running on a separate device, using either KISS-over-TCP or KISS-over-UART. It runs a scripted set of BBS command tests and prints a pass/fail summary to logs.

## What it tests

- BBS banner and prompt
- Configuration mode entry and query
- Info, heard list, and message listing
- Posting, reading, and deleting messages
- Private and bulletin message flows
- Unknown command handling
- Session disconnect and cleanup

## How to use

1. Build and flash to a test ESP32 device.
2. Configure transport (TCP or UART) and connection parameters in menuconfig.
3. Start the BBS (esp-tnc) on another device.
4. Run the test; results and pass/fail summary are printed to the log.
   - Set `TEST_BBS_REMOTE_HOST` (IP/hostname of BBS KISS TCP server)
   - Set `TEST_BBS_REMOTE_TCP_PORT` (default 8100)

TCP transport note:

If the BBS device is using the default fallback AP behavior from `esp-ax25`,
the tester should use:
- `TEST_BBS_WIFI_SSID=ESP-AX25`
- `TEST_BBS_WIFI_PASSWORD=` (empty)
- `TEST_BBS_REMOTE_HOST=192.168.4.1`

If the BBS device is configured to join an existing WiFi network as a station,
set these values to that real network and to the BBS device's actual IP address
instead.

3. **For UART transport**:
   - Set `TEST_BBS_UART_NUMBER` (0–2, typically 1)
   - Set `TEST_BBS_UART_BAUD` (e.g., 9600, 19200)
   - Set `TEST_BBS_UART_TXD_PIN` (GPIO → TNC RXD)
   - Set `TEST_BBS_UART_RXD_PIN` (GPIO ← TNC TXD)

4. **Callsigns**:
   - `TEST_BBS_LOCAL_CALLSIGN` (tester's callsign)
   - `TEST_BBS_REMOTE_CALLSIGN` (BBS callsign)

5. **CONFIG coverage mode**:
   - Leave `TEST_BBS_SYSOP_SECRET` empty to verify no-auth `CONFIG` entry against a BBS with an empty `bbs.sysop_secret`
   - Set `TEST_BBS_SYSOP_SECRET` to the BBS SYSOP secret to verify authenticated `CONFIG` challenge/response entry

## Build / flash

```bash
cd test_bbs
idf.py menuconfig     # Select transport and configure
idf.py set-target esp32c3
idf.py build
idf.py -p <PORT> flash monitor
```

## Example: TCP to bbs on the same network

```bash
idf.py menuconfig
# → Choose TEST_BBS_KISS_TCP
# → Set SSID/password
# → Set remote host/port to match your BBS ESP32
idf.py build && idf.py -p /dev/ttyUSB0 flash monitor
```

## Example: UART to local KISS TNC

```bash
idf.py menuconfig
# → Choose TEST_BBS_KISS_UART
# → Set UART port (1), pins (TX=17, RX=16), baud (9600)
idf.py build && idf.py -p /dev/ttyUSB0 flash monitor
```
