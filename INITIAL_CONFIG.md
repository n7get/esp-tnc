# Initial ESP-TNC Setup

This guide walks through the first-time setup of an `esp-tnc`.

Use it when the device is new or when you are unsure what runtime settings are currently stored. The goal is to get to a known-good baseline first, then add optional features afterward.

For the full command set and more detail on authentication, see `README.md`.

## What This Guide Covers

The basic first-time path is:

1. Connect to the ESP-TNC WiFi AP.
2. Open a terminal session to the default BBS callsign.
3. Enter `CONFIG` mode.
4. Change the minimum required settings.
5. `save`, `reboot`, reconnect, verify, and `commit`.

After that baseline is working, you can optionally:

- enable beacons
- move the device onto your normal WiFi network
- temporarily switch the UART into `TERM` mode to configure an attached serial
  TNC

## Before You Start

You will need:

- A powered and running ESP-TNC.
- A computer or tablet with WiFi.
- A terminal program that can open an AX.25 connected session. This guide uses
  EasyTerm from UZ7HO:
  https://uz7.ho.ua/packetradio.htm
- The callsign you want the BBS to use.
- If you plan to protect `SYSOP` and `CONFIG` access, the secret you want to
  assign to `bbs.sysop_secret`.

## Factory Defaults

For an unpersonalized device, assume:

- The default BBS callsign is `N0CALL-1`.
- The device starts a WiFi AP named `ESP-AX25-BBS`.
- That AP has no password unless it was already changed.
- `CONFIG` usually opens directly unless a SYSOP secret was already set.

If a unit was previously configured, any of those values may already be
different.

## Important Runtime Model

Remote configuration uses a staged workflow:

- `set` changes are only staged.
- `save` writes them and starts the rollback safety window.
- `reboot` applies startup-consumed settings.
- `commit` confirms that the rebooted configuration is good and should be kept.

If you do not `commit` before the timeout expires, the device may restore the
previous configuration and reboot. Keep that sequence in mind throughout this
guide.

## UART Modes At A Glance

If you are using an external serial TNC, there are two UART modes to remember:

- `kiss` is the normal operating mode. AX.25 packet data is exchanged with the
  attached TNC using KISS framing.
- `term` is a temporary maintenance mode. The UART becomes a text pass-through
  terminal so you can send the TNC's own setup commands directly.

If you are not using an external serial TNC, you can ignore the UART-specific
steps in this guide.

## Step 1: Join The ESP-TNC WiFi AP

Before you can configure anything, your computer must be connected to the
ESP-TNC WiFi network.

1. Power the ESP-TNC and wait for it to finish booting.
2. Open your computer's WiFi network list.
3. Join `ESP-AX25-BBS`.
4. Confirm your computer received an IP address from the ESP-TNC network.

On a fresh device, this AP is normally open with no password.

## Step 2: Open A BBS Session

This guide uses EasyTerm from UZ7HO:

- https://uz7.ho.ua/packetradio.htm

Configure EasyTerm like this:

1. Open `Settings` -> `Station Setup`.
2. Enter your own callsign in `Terminal callsign`.
3. Set `Host` to `192.168.4.1`.
4. Click `OK`.
5. Click `Connect`.
6. Enter `N0CALL-1` in `Call To`.
7. Wait for the BBS banner and prompt.

You should see the default prompt:

```text
BBS READY>
```

If you cannot connect:

- Make sure you are still joined to the ESP-TNC WiFi AP.
- Make sure you are connecting to `N0CALL-1` for a fresh device.
- If the unit was configured before, its BBS callsign may no longer be
  `N0CALL-1`.

## Step 3: Enter CONFIG Mode

At the BBS prompt, enter:

```text
CONFIG
```

For a fresh device, this usually enters config mode directly.

Expected response:

```text
*** Entering config mode
CONFIG READY>
```

If the device instead returns a `CHAL ...` line, a SYSOP secret is already
configured. You must complete the challenge/response login before you can
continue.

## Step 4: Set The Minimum Required Configuration

Do the required identity settings first. Only set extra values that you are
ready to verify after reboot.

### Required: Set The BBS Callsign

Set the BBS identity you actually want to use:

```text
set bbs.callsign=<your BBS callsign>
```

Example:

```text
set bbs.callsign=WA7XYZ-1
```

### Recommended: Protect SYSOP And CONFIG Access

If you want to require challenge/response authentication for future `SYSOP` and
`CONFIG` sessions, set a secret now:

```text
set bbs.sysop_secret=<your secret>
```

Example:

```text
set bbs.sysop_secret=replace-this-with-your-own-secret
```

This is optional, but it is usually a good idea once basic access is working.

### Only If You Use A Serial TNC: Set UART Parameters

If the ESP-TNC talks to an external serial TNC, set the UART parameters so they
match your hardware and the TNC's serial configuration.

Typical example:

```text
set uart.enable=true
set uart.type=kiss
set uart.baud=9600
set uart.rx_pin=9
set uart.tx_pin=8
```

What these mean:

- `uart.enable=true` enables the serial TNC path.
- `uart.type=kiss` selects normal packet operation.
- `uart.baud` must match the TNC's serial baud rate.
- `uart.rx_pin` is the ESP-TNC receive pin and must be wired to the TNC's TX.
- `uart.tx_pin` is the ESP-TNC transmit pin and must be wired to the TNC's RX.

If these values are wrong, the ESP-TNC will not be able to communicate with the
attached TNC.

If you are not using a serial TNC, skip this subsection.

### Optional: Configure LED Indicators

If you have LEDs connected to GPIO pins on your hardware, you can enable status
indicators:

```text
set led.pwr_pin=11
set led.bbs_pin=3
```

The LEDs are disabled by default. Adjust the GPIO numbers to match your wiring.

To disable a specific LED without recompiling:

```text
set led.pwr_pin=0
set led.bbs_pin=0
```

LED behavior:

- **PWR LED**: Steady on normally, blinks when the config safety timer is running.
- **BBS LED**: Steady on when connected, blinks when unread messages exist, off when idle

## Step 5: Apply The Changes Safely

Once the minimum settings are staged, use the full apply-and-verify sequence:

```text
save
reboot
```

After reboot:

1. Reconnect your computer to the WiFi
1. Connect to the active BBS callsign using your terminal program.
1. If you changed `bbs.callsign`, use the new callsign, not `N0CALL-1`.
1. Enter `CONFIG` again.
1. Verify the settings look correct.
1. Finish with:

```text
commit
exit
```

Why this order matters:

- `save` writes the pending configuration and starts the rollback timer.
- `reboot` applies settings that are read during startup.
- `commit` confirms that the rebooted configuration is valid and should stay.

If you forget `commit`, the device may roll back to the previous configuration.

## Baseline Complete

At this point, the first-time setup is done if:

- you can reconnect to the BBS
- the active callsign is correct
- any required UART settings are working
- you successfully ran `commit`

Only after that should you move on to optional features.

## Optional Next Step: Enable Beacons

If you want periodic beacon transmissions, set them after the baseline setup is
already working.

Example:

```text
set beacon.every=120
set beacon.source=n7get-7
set beacon.text=ESP-AX25, BBS=N7GET-1 Please test :-)
```

What these do:

- `beacon.every=120` sends a beacon every 120 seconds.
- `beacon.source=n7get-7` sets the source callsign.
- `beacon.text=...` sets the transmitted text.

After changing beacon settings, use the normal `save` / `reboot` / reconnect /
`commit` flow.

## Optional Next Step: Move To Your Normal WiFi Network

Once the device is reachable and basic setup works, you may want it to join your
existing WiFi network instead of relying only on the fallback AP.

Set:

```text
set wifi.sta.ssid=<local SSID>
set wifi.sta.password=<local password>
```

Example:

```text
set wifi.sta.ssid=MyShackWiFi
set wifi.sta.password=replace-with-your-password
```

Then use the normal `save` / `reboot` / reconnect / `commit` cycle.

After reboot, if the WiFi settings are valid, the ESP-TNC may now be reachable
through your normal LAN instead of only through the fallback AP. You will need
to determine its new IP address from your WiFi network.

## Optional: Using the UART as a serial terminal

Use `TERM` only when you need direct maintenance access to an attached serial
TNC.

### Enable TERM Mode

Enter `CONFIG` and set:

```text
set uart.enable=true
set uart.type=term
```

If needed, also verify the UART pins and baud rate before applying the change.

Then run:

```text
save
reboot
```

Reconnect to the BBS.

At that point you have two choices:

- `commit` first if you want `term` mode to stay active beyond the rollback
  window.
- Delay `commit` if this is short maintenance work and you want rollback as a
  safety net.

### Use TERM Mode

Once the device is back up, connect to the BBS and enter:

```text
TERM
```

You should now have a pass-through terminal to the attached TNC. Lines you type
are forwarded to the UART, and the TNC's responses are sent back over your AX.25
session.

Use this to send the TNC's own vendor-specific setup commands, including any
command required to place that TNC into KISS mode.

To leave terminal mode, type:

```text
/exit
```

### Return UART To Normal KISS Mode

After maintenance is complete, reconnect to the BBS, enter `CONFIG`, and set:

```text
set uart.type=kiss
```

Then use the normal apply sequence:

```text
save
reboot
```

Reconnect again, verify the device is behaving correctly, then:

```text
commit
exit
```

If you intentionally skipped `commit` when entering `term` mode, you may be able
to use rollback instead:

1. Re-enter `CONFIG`.
2. Run `revert`.
3. Reboot.

That restores the last saved configuration, which is useful when the previously
committed value was already `uart.type=kiss`.

## Troubleshooting

### I cannot find the ESP-TNC WiFi AP

- Make sure the device is powered and fully booted.
- Check whether it was already configured to join an existing WiFi network.
- Check whether the AP name was changed from the defaults.

### I cannot connect to `N0CALL-1`

- Make sure you are on the ESP-TNC network.
- If the device was already configured, the active BBS callsign may no longer be
  `N0CALL-1`.
- If you already changed `bbs.callsign` and rebooted, connect to the new
  callsign instead.

### `CONFIG` asks for a challenge unexpectedly

- A SYSOP secret is already configured.
- You must complete the challenge/response process before `CONFIG` access is
  granted.

### I changed settings and they did not stick

- `set` only stages the changes.
- You must follow the full `save` / `reboot` / reconnect / `commit` flow.

### I lost contact after changing WiFi settings

- The device may now be reachable on your normal LAN instead of its fallback AP.
- Reconnect your computer to the correct network and then reconnect to the BBS.

### `TERM` is rejected

- `TERM` only works when `uart.type=term` is active.
- Change it in `CONFIG`, then `save`, `reboot`, reconnect, and try again.

### I forgot to put the UART back into KISS mode

- Re-enter `CONFIG`.
- Set `uart.type=kiss`.
- `save`, `reboot`, reconnect, and `commit`.

## Summary

The safest first-time setup flow is:

1. Join the fallback AP.
2. Connect to `N0CALL-1`.
3. Enter `CONFIG`.
4. Set your callsign and any required UART settings.
5. `save` and `reboot`.
6. Reconnect using the correct BBS callsign.
7. Verify the device works as expected.
8. `commit` the configuration.

Once that baseline is stable, add beacons, LAN WiFi access, or temporary
`TERM` mode as separate follow-up changes.
