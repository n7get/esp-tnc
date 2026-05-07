# ESP-TNC Configuration Parameters

All runtime configuration is stored in NVS and managed through Config Mode (`CONFIG` BBS command). Use `show all` at the `CONFIG READY>` prompt to see every parameter and its current effective value.

## Using Config Mode

```text
get <parameter>              show current value
set <parameter>=<value>      stage a change
clear <parameter>            stage reset to default
show                         list all non-default values
show all                     list every parameter
show <prefix>                list parameters by prefix (e.g. wifi. or net.)
config                       print set commands for all non-default values
save                         write staged changes; starts rollback timer
commit                       confirm saved changes; cancels rollback timer
revert                       restore last backup snapshot
reboot                       restart to apply saved changes
exit                         leave config mode; discard unsaved staged changes
```

Changes take effect at the next reboot unless noted otherwise. Always follow the `save` / `reboot` / reconnect / `commit` sequence to make changes permanent.

---

## BBS Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `bbs.callsign` | string | `N0CALL-1` | | AX.25 callsign the BBS listens on. |
| `bbs.sysop_secret` | string | _(empty)_ | | Challenge/response secret for `SYSOP` and `CONFIG` access. Empty = no authentication required. **Hidden in `show all` output.** |

---

## Beacon Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `beacon.destination` | string | `BEACON` | | Destination callsign of the transmitted beacon UI frame. |
| `beacon.every` | int | `0` | 0–10080 | Beacon interval in seconds. `0` disables beaconing. |
| `beacon.source` | string | _(empty)_ | | Source callsign used in the beacon frame. Must be set for beaconing to work. |
| `beacon.text` | string | `ESP-AX25` | | Beacon information text transmitted in the UI frame payload. |
| `beacon.via` | string | _(empty)_ | | Optional digipeater path for beacon frames (e.g. `WIDE1-1`). |

---

## Digipeater Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `digi.callsign` | string | _(empty)_ | | Callsign the digipeater aliases. Empty = digipeater disabled (regardless of compile-time enable). |

---

## LED Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `led.pwr_pin` | int | `0`¹ | 0–48 | GPIO pin for the power/status LED. `0` disables the LED. Blinks when the config rollback safety timer is active. |
| `led.bbs_pin` | int | `0`¹ | 0–48 | GPIO pin for the BBS status LED. `0` disables the LED. Steady on when a client is connected; blinks when unread messages exist. |

¹ Compile-time default set by `CONFIG_TNC_PWR_LED_GPIO` / `CONFIG_TNC_BBS_LED_GPIO` in menuconfig (default `0`).

---

## UART Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `uart.enable` | bool | `true` | 0,1 | Enable the UART path. Set to `false` if no serial TNC is attached. |
| `uart.type` | enum | `term` | `kiss`, `term` | UART operating mode. `kiss` = normal KISS packet data; `term` = text pass-through terminal for TNC maintenance. |
| `uart.baud` | enum | `9600` | 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 | UART baud rate. Must match the attached TNC's serial configuration. |
| `uart.no` | int | `1` | 0–2 | ESP32 UART peripheral number. |
| `uart.rx_pin` | int | `7` | 0–48 | GPIO receive pin. Wire to the TNC's TX. |
| `uart.tx_pin` | int | `6` | 0–48 | GPIO transmit pin. Wire to the TNC's RX. |

---

## Wi-Fi Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `wifi.ap.ssid` | string | `ESP-AX25-BBS` | | SSID of the fallback Wi-Fi access point the device broadcasts. |
| `wifi.ap.password` | string | _(empty)_ | | Password for the fallback AP. Empty = open network. **Hidden in `show all` output.** |
| `wifi.sta.ssid` | string | _(empty)_ | | SSID of the Wi-Fi network to join as a client. Empty = do not attempt STA connection. |
| `wifi.sta.password` | string | _(empty)_ | | Password for the STA network. **Hidden in `show all` output.** |
| `wifi.sta.hostname` | string | `esp-ax25` | | mDNS/DHCP hostname when connected as a STA client. |
| `wifi.sta.connect_timeout_ms` | int | `15000` | 0–600000 | Milliseconds to wait for a STA connection before falling back to AP mode. |
| `wifi.sta.max_waits` | int | `4` | 0–64 | Number of connection retry cycles before giving up on STA. |

---

## Network: AGWPE Server Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `net.agwpe.port` | int | `8000` | 1–65535 | TCP port the AGWPE server listens on. |
| `net.agwpe.max_clients` | int | `4` | 1–32 | Maximum number of simultaneous AGWPE TCP connections. |
| `net.agwpe.server.max_conns` | int | `1` | 1–32 | Maximum simultaneous AX.25 connected-mode sessions per AGWPE client. |
| `net.agwpe.conn_task_stack` | int | `6144` | 1024–65536 | Stack size in bytes for each AGWPE connection task. |

---

## Network: KISS TCP Server Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `net.kiss.port` | int | `8100` | 1–65535 | TCP port the KISS server listens on. |
| `net.kiss.max_conns` | int | `4` | 1–16 | Maximum number of simultaneous KISS TCP client connections. |
| `net.kiss.server.promiscuous` | bool | `false` | 0,1 | When `true`, each KISS TCP client receives all frames on the router regardless of destination (promiscuous mode). When `false`, clients only receive frames addressed to the callsign of their first transmitted frame (dynamic mode). |
| `net.kiss.accept_task_priority` | int | `5` | 1–25 | FreeRTOS priority for the KISS TCP accept task. |
| `net.kiss.accept_task_stack` | int | `4096` | 1024–65536 | Stack size in bytes for the KISS TCP accept task. |
| `net.kiss.conn_task_priority` | int | `5` | 1–25 | FreeRTOS priority for each KISS TCP connection task. |
| `net.kiss.conn_task_stack` | int | `6144` | 1024–65536 | Stack size in bytes for each KISS TCP connection task. |
| `net.kiss.tx_queue_depth` | int | `4` | 1–64 | Outbound frame queue depth per KISS TCP client connection. |
| `net.kiss.tx_task_priority` | int | `5` | 1–25 | FreeRTOS priority for each KISS TCP client transmit task. |
| `net.kiss.tx_task_stack` | int | `3072` | 1024–65536 | Stack size in bytes for each KISS TCP client transmit task. |
| `net.kiss.server_tx_queue_depth` | int | `4` | 1–64 | Outbound frame queue depth for the KISS server accept path. |
| `net.kiss.server_tx_task_priority` | int | `5` | 1–25 | FreeRTOS priority for the KISS server transmit task. |
| `net.kiss.server_tx_task_stack` | int | `3072` | 1024–65536 | Stack size in bytes for the KISS server transmit task. |

---

## Network: Monitor TCP Server Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `net.monitor.port` | int | `8200` | 1–65535 | TCP port the monitor server listens on. Streams decoded frame traffic to connected clients. |
| `net.monitor.max_conns` | int | `1` | 1–8 | Maximum simultaneous monitor TCP connections. |
| `net.monitor.conn_task_priority` | int | `5` | 1–25 | FreeRTOS priority for each monitor connection task. |
| `net.monitor.conn_task_stack` | int | `3072` | 1024–65536 | Stack size in bytes for each monitor connection task. |
| `net.monitor.tx_queue_depth` | int | `4` | 1–64 | Outbound frame queue depth per monitor connection. |
| `net.monitor.tx_task_priority` | int | `5` | 1–25 | FreeRTOS priority for each monitor transmit task. |
| `net.monitor.tx_task_stack` | int | `3072` | 1024–65536 | Stack size in bytes for each monitor transmit task. |

---

## Network: Log TCP Server Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `net.log.port` | int | `8300` | 1–65535 | TCP port the log server listens on. Streams ESP-IDF log output to connected clients. |
| `net.log.max_conns` | int | `1` | 1–8 | Maximum simultaneous log TCP connections. |
| `net.log.conn_task_priority` | int | `5` | 1–25 | FreeRTOS priority for each log connection task. |
| `net.log.conn_task_stack` | int | `3072` | 1024–65536 | Stack size in bytes for each log connection task. |
| `net.log.queue_depth` | int | `32` | 1–1024 | Log message queue depth. Increase if log output is being dropped under load. |

---

## AX.25 Router Parameters

These parameters are rarely changed. They tune the internal frame router that connects all active services.

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `ax25.router.max_ports` | int | `16`¹ | 2–32 | Maximum number of router ports (UART PHY, KISS TCP clients, AGWPE sessions, BBS, monitor, etc.). Increase if the device runs out of ports at startup. |
| `ax25.router.queue_depth` | int | `8` | 2–32 | Per-port inbound frame queue depth inside the router. |

¹ Compile-time default set by `CONFIG_AX25_ROUTER_MAX_PORTS` in menuconfig (default `16`).

---

## Notes

- **Hidden parameters** (`bbs.sysop_secret`, `wifi.ap.password`, `wifi.sta.password`) do not appear in `show all` output to prevent credential exposure over the air.
- Parameters that affect task stack sizes and priorities take effect on the next reboot.
- The compile-time menuconfig (`idf.py menuconfig`, menu `ESP-TNC BBS`) sets factory defaults for LED GPIO pins and router max ports. Runtime `set` commands override those defaults and persist to NVS.
