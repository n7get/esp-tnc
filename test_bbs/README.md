# test_bbs

Automated AX.25 BBS test application for a second ESP32.

This app supports two KISS transport modes:

- **TCP KISS**: Connect over WiFi to a remote KISS TCP server, or
- **UART KISS**: Connect via UART to a local KISS TNC

Once connected, it opens an AX.25 connected session to the BBS, runs a scripted
set of BBS command tests, and prints a pass/fail summary to logs.

## What it tests

- BBS banner and prompt (`BBS> `)
- `CONFIG` entry, read-only query, and exit
- `I` (info)
- `J` (heard list)
- `LL` (list messages)
- `SB` post flow, including `Subject:` and `/EX` finish
- banner unread-count summary and state after reconnect/read/delete
- `L` list contains posted message
- `LM` mine-only filtering
- `R <id>` reads posted message
- `K <id>` deletes posted message
- `R <id>` after delete reports not found
- `SP <valid>` private post flow, visibility filtering, and access denial for non-addressed reads
- `SP <invalid>` validation
- unknown command help output
- `B` disconnect behavior
- Cleanup of test-created messages

## Configure

Run menuconfig in this app folder and:

1. **Transport mode**: Choose `TEST_BBS_KISS_TCP` or `TEST_BBS_KISS_UART` (mutually exclusive)

2. **For TCP transport**:
   - Set `TEST_BBS_WIFI_SSID` and `TEST_BBS_WIFI_PASSWORD`
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
