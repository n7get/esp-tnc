# Rigcon-232 Notes

## Cable for interfacing to a serial TNC

If you are connecting a Rigcon-232 module to a TNC that uses a DB-25 serial port, wire the 3.5 mm TRRS plug as follows:

| Rigcon-232 TRRS contact | DB-25 pin | Note |
| --- | ---: | --- |
| Tip | 2 | RX Serial data |
| Ring 1 | 3 | TX Serial data |
| Ring 2 | N/C | |
| Sleeve | 7 | Signal ground |

On some TNCs you may also need to short pins 4 and 5 together so
the port sees the expected hardware handshaking state.

## UART configuration

The Rigcon-232 uses the following pin assignments for TX and RX:
```text
set uart.rx_pin=9
set uart.tx_pin=8
```

## Power connector

The power connector is a 5.5x2.1 barrel with center positive.  The voltage range is 3.8 to 32.

## LED GPIO Pin Assignments

The Rigcon-232 module supports optional status LEDs connected to the following GPIO pins:

```text
set led.pwr_pin=11      # PWR LED - Power/Status indicator
set led.bbs_pin=3       # BBS LED - Connection/Activity indicator
```

These are the default GPIO assignments and can be changed at runtime via configuration.

**PWR LED (GPIO 11)** shows:
- Steady on: Normal operation
- Blinking (1s on/off): Configuration safety timer active

**BBS LED (GPIO 3)** shows:
- Steady on: Client connected to BBS
- Blinking (1s on/off): No client connected, unread messages exist
- Off: No activity

To disable an LED, set its GPIO to 0:

```text
set led.pwr_pin=0
set led.bbs_pin=0
```
