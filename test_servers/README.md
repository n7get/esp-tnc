
# test_server

Automated test app for exercising an external esp-tnc server (running on a separate device) with scripted BBS flows over TCP transports (KISS and AGWPE).  This test was developed to be flashed to multiple devices and executed simultaneously to stress test an esp-tnc.  

## What it tests

- Connects to esp-tnc via KISS-over-TCP and AGWPE-over-TCP
- Establishes AX.25/BBS session
- Sends basic BBS commands (e.g., heard list, disconnect)
- Alternates between KISS and AGWPE if both are enabled

## How to use

1. Build and flash to a test ESP32 device.
2. Configure local/remote callsigns, target host/IP, and TCP ports in menuconfig.
3. Enable desired transports (KISS, AGWPE) in menuconfig.
4. Start the BBS (esp-tnc) on another device.
5. Run the test; results and pass/fail summary are printed to the log.

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
