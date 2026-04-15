# test_server

`test_server` is a standalone ESP-IDF app for exercising an external `esp-tnc`
server with a scripted BBS flow over TCP transports.

Supported transports:
- KISS-over-TCP
- AGWPE-over-TCP

Both transports are TCP-only in this app. There is no UART path.

## What It Does

For each session, the app:
1. Connects to the target `esp-tnc` transport endpoint.
2. Establishes connected-mode AX.25/BBS session.
3. Waits for the configured BBS prompt prefix.
4. Sends `j\r`.
5. Waits for the next configured BBS prompt prefix.
6. Sends `b\r` and disconnects.

## Transport Enable/Disable Behavior

Configured in menuconfig:
- `TEST_SERVER_ENABLE_KISS_CLIENT`
- `TEST_SERVER_ENABLE_AGWPE_CLIENT`

Runtime behavior:
- Both disabled: app logs an error and exits.
- Only one enabled: app runs only that transport.
- Both enabled: app alternates session-by-session between KISS and AGWPE.

## Key Configuration

Open menuconfig in `test_server` and set:
- `AX.25 Test Server Configuration -> Local Callsign`
- `AX.25 Test Server Configuration -> Remote Callsign`
- `AX.25 Test Server Configuration -> Target Host/IP`
- `AX.25 Test Server Configuration -> KISS TCP Port`
- `AX.25 Test Server Configuration -> AGWPE TCP Port`
- `AX.25 Test Server Configuration -> Session Count`
- `AX.25 Test Server Configuration -> Connect timeout (ms)`
- `AX.25 Test Server Configuration -> Prompt read timeout (ms)`
- `AX.25 Test Server Configuration -> BBS prompt prefix`
- Wi-Fi credentials under the same menu (`Wi-Fi STA SSID`, `Wi-Fi STA Password`)

## Build / Flash / Monitor

From this directory:

```bash
cd /Users/rna/esp/components/esp-ax25/test_server
source "$HOME/esp/esp-idf/export.sh"
```

Set target once (recommended if your board is ESP32-S3):

```bash
idf.py set-target esp32s3
```

Build:

```bash
idf.py build
```

Flash and monitor:

```bash
idf.py -p <PORT> flash monitor
```

## Expected Logs

Typical milestones:
- `Starting session X/Y via KISS/TCP`
- `Starting session X/Y via AGWPE/TCP`
- `BBS: ...` where the line starts with the configured prompt prefix
- `All test_server sessions completed successfully`

Failure examples:
- both transports disabled
- timeout waiting for TCP socket
- timeout waiting for AX.25/AGWPE connect event
- timeout waiting for BBS prompt
