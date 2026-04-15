# esp-tnc

ESP-TNC is a standalone AX.25 application for ESP32 microcontrollers.

ESP-TNC allows up to a configurable number of simultaneous connections.
For example a RMS Packet, RMS Express and UZ7HO's Easyterm can all share 
a single KISS TNC+radio.  

ESP-TNC does not connect directly to the radio, it requires a KISS capable 
radio interface such as a KAM style TNC or NinoTNC type of device.

Features:

- A BBS
- Can connect as a WiFi client (STA) or act as an Access Point (AP)
- KISS over UART
- KISS over TCP
- AGWPE TCP server
- Digipeater
- Supports sending beacons

## Using the BBS

ESP-TNC has a built in BBS. An operator can connect to
 the configured BBS callsign and use the command set documented below
to list, read, and post messages.

The BBS local callsign is read from config key `bbs.callsign`.

## BBS Commands

Main commands:

- `I`: show BBS info
- `H` or `?`: help
- `J`: heard list
- `L`: list readable messages
- `LL <n>`: list last N messages
- `LM`: list messages related to current user
- `R <n>`: read message
- `S <dest>`: send (callsign => private, other => bulletin/topic)
- `SP <call>`: send private message
- `SB <dest>`: send bulletin
- `K <n>`: delete message (own or SYSOP)
- `SYSOP`: start SYSOP authentication
- `CONFIG`: enter config mode (after auth if secret is set)
- `TERM`: enter UART terminal mode (after auth)
- `B`: disconnect session

## Message compose flow:

1. Issue `S`, `SP`, or `SB`
1. Enter subject when prompted
1. Enter message body lines
1. End body with `/EX` on its own line

## Over-The-Air Configuration

`esp-tnc` supports remote configuration over an active AX.25 BBS session using
the `CONFIG` command.

Config mode commands:
  - `set <parameter>=<value>`   Stage a value change (not yet persistent)
  - `get <parameter>`           Show effective value (includes staged change)
  - `clear <parameter>`         Stage reset to schema default
  - `show`                      Show changed values only
  - `show all`                  Show all parameters
  - `show <prefix>`             Show parameters by prefix (for example: wifi. or net.)
  - `config`                    Print `set` commands for all non-default values
  - `save`                      Write staged changes and starts the commit timeout period
  - `commit`                    Confirm saved changes and cancel the commit timer
  - `revert`                    Restore last backup snapshot
  - `reboot`                    Restart now to apply saved/reverted values
  - `exit`                      Leave config mode and discard unsaved staged changes

**NOTES:**
  - The configuration is loaded at boot time.
  - A commit must be done to make changes permanent.
  - If a commit is not completed before the commit timeout period expires (5 minutes), the previous configuration is restored and the device is rebooted.  This is a safety measure to ensure that access is not accidently lost.
  - If you reboot before saving, pending edits may be lost.

Typical workflow:

1. Connect to the BBS callsign.
1. Enter config mode with `CONFIG`.
1. Authenticate by completing the challenge/response step, see below.  Required when a SYSOP secret is configured.
1. Inspect/modify the configuration values.  
1. Persist any changes with `save`.  
1. `reboot` restarts the device and starts the commit timer. 
1. Reconnect using the BBS callsign and re-enter config mode.
1. Review any changes and `commit` to make this configuration permanent.
1. Use `exit` to leave config mode and return to the BBS.

## Using TERM to Configure a KISS TNC

`TERM` provides a pass-through UART terminal to an attached TNC so you can
send device-specific command lines (for example setup commands KAM-style unit).

Prerequisites:

The following configuration must be set:
- `uart.enable=true`
- `uart.type=term`

Normally `uart.type=kiss` for KISS data to and from the TNC.

Workflow:

1. Connect to the BBS callsign.
1. Enter `CONFIG` mode and `set uart.type=term`.
1. `save`/`reboot`/`commit` Note: if you can complete the following within the 
commit timeout period you can skip the commit.
1. Enter terminal mode with `TERM`.
1. Send TNC configuration lines exactly as you would on a local serial console.
   ESP-TNC forwards each line to UART and forwards UART output back to your AX.25
   session.
1. Send `/exit` to leave terminal mode and return to BBS command mode.
1. Restore the `uart.type=kiss` configuration parameter.  
    1. If you did the `commit` above, go through the `CONFIG`/`SET uart.type=kiss`/`save`/`reboot` ... `commit` process.
    1. Otherwise if you skipped the commit, re-enter the `CONFIG` and `revert`/`reboot` to 
    restore `uart.type=kiss`.

Operational notes:

- If `uart.type` is not `term`, `TERM` is rejected.
- Terminal mode is intended for direct TNC maintenance, not normal AX.25 data
  path operation.
- Any save/write command syntax is TNC-specific; use your TNC vendor command set
  while in `TERM`.
- `/reset` sends the KISS mode reset sequence (C0 F0 C0) to the TNC.

## LED Indicators

ESP-TNC supports optional LED indicators for device status:

**PWR LED (Power/Status Indicator):**
- Steady on during normal operation
- Blinks off and on when configuration safety timer active 
- GPIO pin configured via `led.pwr_pin`

**BBS LED (BBS Connection Indicator):**
- Steady on when a client is connected to the BBS
- Blinks off and on when no client is connected but unread messages exist in the BBS
- GPIO pin configured via `led.bbs_pin` 

**Disabling LEDs:**
Set either LED GPIO to `0` to disable that LED without recompiling:
```
set led.pwr_pin=0
set led.bbs_pin=0
```

## SYSOP Authentication

`esp-tnc` uses challenge/response SYSOP authentication with:

- challenge timeout
- authenticated session timeout
- lockout window after repeated failures
- configurable min/max secret length

These are configured under `ESP-TNC BBS` in menuconfig.

## Challenge/Response Process (SYSOP and CONFIG)

Both `SYSOP` and `CONFIG` rely on the same challenge/response engine when a
non-empty `bbs.sysop_secret` is configured.

Flow:

1. User sends `SYSOP` or `CONFIG`.
1. Device issues a challenge line in this format:
   - `CHAL i1 i2 i3 i4`
1. `i1..i4` are 1-based positions into the configured SYSOP secret.
1. User sends a 6-character response containing the challenged secret
  characters plus two random characters.
1. On success:
   - `SYSOP` path: authenticated session starts and command prompt returns.
   - `CONFIG` path: authenticated session starts and config mode is entered
     immediately.

Operational details:

- If a challenge is active, `SYSOP NEW` refreshes it.
- Challenge responses must arrive before the challenge timeout expires.
- Failed attempts increment a counter; after max attempts, SYSOP is locked out
  for the configured lockout interval.
- Authenticated SYSOP sessions expire after the configured idle timeout.
- If `bbs.sysop_secret` is empty, SYSOP/CONFIG access is bypass-authenticated
  (no challenge is required).

This repo has a python script: tools/sysop_challenge_response.py, that makes the
Challenge/Response Process more manageable.  The SYSOP secret is saved into a 
file called ~/.sysop_secret.  The operator can copy and paste the challenge
numbers and sysop_challenge_response.py will respond with an appropriate
response.  

## Troubleshooting

- Build fails resolving components:
  - Verify network access and `main/idf_component.yml` dependency coordinates.
- BBS storage mount failure:
  - Confirm `partitions.csv` includes `bbs_data` and menuconfig label matches.
- No UART traffic:
  - Verify `uart.enable`, UART pins, and baud settings.
- No TCP services:
  - Confirm Wi-Fi startup succeeds and network config is valid.

## License

This project reuses modules and APIs from `esp-ax25`. Review licenses in both
repositories before distribution.
