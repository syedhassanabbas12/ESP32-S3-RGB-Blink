# ESP32-S3 Home Automation — Claude Code Project Guide

## 1) Project Overview

A DIY / learning **home-automation & IoT** build on an **ESP32-S3** microcontroller,
using **MQTT** as the messaging backbone.

Owner goals:
- learn IoT and embedded hands-on (owner is **new to hardware/embedded** — explain wiring
  basics and go step-by-step)
- build home automations (schedules, rules, phone control)
- gather **power-consumption** data from appliances
- eventually control real mains loads (room light, ceiling fan)

The work is **staged deliberately**: prove each layer on safe low-voltage hardware (LEDs)
before touching relays, and touch relays before touching lethal mains voltage. See
§3 Roadmap.

This repo (`ESP32-S3-RGB-Blink`) currently contains **only the device firmware**
(Stage 1). The phone app, automation engine, and data pipeline are separate future
efforts (§5).

## 2) Current Status

| Layer | State |
|---|---|
| 3 LEDs controllable over MQTT (`red`/`green`/`blue`) | ✅ **Done** (2026-07-04), full round-trip verified |
| Mosquitto broker on Mac, LAN + WebSockets listeners | ✅ Running (`1883` TCP, `9001` WS) |
| Phone control (React Native app over MQTT-WebSockets) | ✅ **Done** (2026-07-04), two-way sync verified |
| Relays (dry-run via optocouplers) | ⏳ Software done — firmware+app channels added & round-trip verified; wiring pending |
| Power metering (PZEM-004T) | ⬜ Not started |
| Mains loads | ⬜ Not started (last, by design) |

"Verified" means: published `home/dev1/red/set on` → LED lit → device echoed
`home/dev1/red/state on`.

## 3) Staged Roadmap

Do these **in order**. Each stage de-risks the next. Live task tracking (checklists,
sub-tasks, done log) lives in [`docs/TODO.md`](docs/TODO.md) — keep it in sync as work lands.

1. **✅ LEDs over MQTT** — 3 LEDs on GPIO 4/5/6, controlled by MQTT `set`, reporting `state`.
2. **⏳ Phone control** — Mosquitto WebSockets listener (`9001`, already configured) + a
   browser test page first, then a **React Native app** (Expo recommended for a newbie).
   The app speaks **MQTT-over-WebSockets** to the broker.
3. **⬜ Relays (dry-run)** — 4-channel relay module driven through **PC817 optocouplers**
   on GPIO 15/16/17/18. Click-test with **nothing wired to the switched side**. Relay
   boards are usually **5V active-LOW**; 3.3V drive is marginal, so use the PC817s and
   **remove the VCC/JD-VCC jumper** for isolation.
4. **⬜ Power metering** — add a **PZEM-004T v3.0** (UART, mains-isolated). The clamp
   meter is manual-only. **Skip the ACS712.**
5. **⬜ Mains loads** — room light, ceiling fan. **LETHAL — do this last**, fully isolated
   with optos and enclosed. A relay gives **on/off only**; **fan speed needs a TRIAC**, not
   a relay. See §13 Safety.

## 4) Hardware

- **Board:** ESP32-S3-N16R8 — 16 MB flash / **8 MB octal PSRAM**
  - PlatformIO board id: `4d_systems_esp32s3_gen4_r8n16`
- Breadboard, red/green/blue LEDs, **220Ω** series resistors
- 4-channel relay module, **PC817** optocouplers
- Digital clamp meter (manual power readings)
- Future: **PZEM-004T v3.0** energy monitor

### GPIO usage & reserved pins

| Purpose | GPIO |
|---|---|
| `red` LED | 4 |
| `green` LED | 5 |
| `blue` LED | 6 |
| (planned) relays 1–4 | 15, 16, 17, 18 |

**Do NOT use these pins** (they break flash/PSRAM, boot, or built-in peripherals on the
N16R8):
- **26–37** — reserved for octal flash / PSRAM
- **0, 3, 45, 46** — strapping pins (affect boot mode)
- **19, 20** — native USB
- **43, 44** — default UART0 (serial console / logs)

## 5) Architecture

MQTT is the single backbone. Everything talks to the broker, not to each other.

```
  [ESP32-S3 firmware, C++]                 [React Native app, JS]
        │  MQTT/TCP 1883                          │  MQTT/WebSockets 9001
        └──────────────┬──────────────────────────┘
                       ▼
              [Mosquitto broker]   ← runs on the Mac (192.168.173.170)
                       ▲
        ┌──────────────┴───────────────┐
   [Node-RED automations, JS]     [InfluxDB + Grafana]   (future: power data)
```

Language split (intentional):
- **Firmware → C++** (PlatformIO / Arduino framework + `PubSubClient`)
- **Everything else → JavaScript** (React Native app, Node-RED flows, dashboards)

Infrastructure:
- **Broker:** Mosquitto on the owner's Mac at **`192.168.173.170`**
  - config: `/opt/homebrew/etc/mosquitto/lan.conf`
  - listeners: `1883` (plain TCP, for the ESP32) and `9001` (WebSockets, for the app)
  - **anonymous allowed, LAN only** (no auth yet — do not expose to the internet)
- **Network:** phone hotspot **"Galaxy A72"**. Must be **2.4 GHz** (ESP32-S3 has no 5 GHz
  radio). The ESP32 and the Mac must be on the **same** network.

## 6) MQTT Topic Contract

Base pattern: `home/<device-id>/…`. Device id is `dev1` (unique per board).

| Topic | Direction | Payload | Notes |
|---|---|---|---|
| `home/dev1/status` | device → all | `online` / `offline` | **retained**, set as Last-Will |
| `home/dev1/<ch>/set` | app → device | `on` / `off` / `toggle` | command |
| `home/dev1/<ch>/state` | device → app | `on` / `off` | **retained** (new subscribers get last value) |

`<ch>` is a channel name (`red`, `green`, `blue`, later `relay1`, …). The device
subscribes to `home/dev1/+/set` (the `+` wildcard matches any single channel).

Design intent:
- **Retained `state`** so a freshly opened app immediately knows each output's value.
- **Retained Last-Will `status`** so subscribers know if the device dropped off the network.
- On (re)connect the device republishes every channel's `state` to resync clients.

Quick manual test (broker must be running):
```bash
# watch everything
mosquitto_sub -h 192.168.173.170 -t 'home/#' -v
# turn the red LED on
mosquitto_pub -h 192.168.173.170 -t 'home/dev1/red/set' -m on
```

## 7) Repository Layout

```
ESP32-S3-RGB-Blink/
├── platformio.ini          # board, framework, libs, monitor speed
├── src/
│   └── main.cpp            # the entire Stage-1 firmware
├── include/                # (empty) project headers
├── lib/                    # (empty) private libraries
├── test/                   # (empty) PlatformIO unit tests
├── .vscode/                # editor config (mostly gitignored)
├── .gitignore              # ignores .pio build dir, etc.
└── CLAUDE.md               # this guide
```

Broker config lives **outside** this repo (it's a Mac system path, added as an extra
working directory): `/opt/homebrew/etc/mosquitto/lan.conf`.

## 8) Firmware Walkthrough (`src/main.cpp`)

Single-file Arduino-style program. Key pieces:

- **Config block (top)** — Wi-Fi SSID/password, broker host/port, MQTT user/pass,
  `DEVICE_ID`. Edit these per board / per network.
- **`Channel` struct + `channels[]`** — each controllable output is `{ name, pin, state }`.
  **Add new outputs (relays) here** — this is the main extension point (§11).
- **`logf()`** — the logger. Prints every line with an uptime prefix (§9).
- **`mqttStateStr()` / `resetReasonStr()`** — turn numeric MQTT rc codes and boot-reset
  reasons into plain English in the logs.
- **`applyChannel()`** — drives the GPIO, logs the change, and publishes the new `state`.
- **`publishState()`** — publishes retained `home/dev1/<ch>/state` (and logs the tx).
- **`onMessage()`** — MQTT callback; logs every received message, then matches incoming
  `…/<ch>/set` topics and applies `on` / `off` / `toggle`.
- **`connectWiFi()` / `connectMQTT()`** — blocking (re)connect loops with progress logs.
  `connectMQTT()` sets the retained Last-Will, publishes `online`, subscribes to
  `home/dev1/+/set`, and resyncs all states.
- **`loop()`** — reconnects Wi-Fi/MQTT if dropped, calls `mqtt.loop()` (must run often —
  **don't add long `delay()`s here**), and emits a 30-second heartbeat.

## 9) Serial Logs — Seeing What the Device Is Doing

The firmware narrates itself over **USB serial at 115200 baud**. Open the monitor:

```bash
~/.platformio/penv/bin/pio device monitor -b 115200
# (or click "Monitor" in the VS Code PlatformIO toolbar)
```

Every line is prefixed with the **uptime** since boot, as `[seconds.millis]`, so you can
see ordering and timing at a glance. Tap **RST** on the board to restart and watch the
boot banner from the top.

### Log tags

| Tag / line | Meaning |
|---|---|
| `==== ... booting ====` + `device id / chip / flash / free heap / reset` | One-time **boot banner**. `reset` says *why* it rebooted (`power-on`, `panic`, `brownout`, …) — a `panic`/`brownout` is a red flag. |
| `WiFi: ...` | Wi-Fi connect / reconnect. On success logs **IP, RSSI (signal), MAC**. Repeated "still trying" = wrong SSID/password or not a 2.4 GHz network. |
| `MQTT: connecting / connected / subscribed` | Broker connection lifecycle. `connect failed rc=… (reason)` explains failures in English (e.g. `rc=-2` = broker down / wrong IP / wrong network). |
| `MQTT: rx  <topic> = "<payload>"` | A message **arrived** from the broker (someone published a command). |
| `MQTT: tx  <topic> = <value>` | The device **published** a state update. |
| `CH:   <name> (gpio N) -> ON/off` | An output **physically changed** (LED/relay switched). |
| `alive  uptime=… heap=… rssi=…` | **Heartbeat every 30 s** — proves the device is alive and shows free memory + signal. A steadily falling `heap` hints at a memory leak. |

### Example — boot, then one command round-trip

```
[     0.312] ==== ESP32-S3 IoT node booting ====
[     0.313] device id : dev1
[     0.314] chip      : ESP32-S3 rev0, 2 core(s)
[     0.315] flash     : 16 MB
[     0.317] reset     : power-on
[     0.318] CH:   red on gpio 4 -> off
[     0.321] WiFi: connecting to "Galaxy A72" ...
[     2.845] WiFi: connected  ip=192.168.173.55  rssi=-58 dBm  mac=...
[     2.900] MQTT: connecting to 192.168.173.170:1883 as "dev1" ...
[     2.951] MQTT: connected
[     2.952] MQTT: subscribed to home/dev1/+/set
[     2.953] MQTT: tx  home/dev1/red/state = off
[    18.400] MQTT: rx  home/dev1/red/set = "on"       ← command came in
[    18.401] CH:   red (gpio 4) -> ON                 ← LED switched
[    18.402] MQTT: tx  home/dev1/red/state = on       ← state echoed back
[    30.318] alive  uptime=30s  heap=273900  rssi=-57 dBm
```

### Log-based troubleshooting

| Symptom in the log | Likely cause / fix |
|---|---|
| Garbled / random characters | Monitor baud ≠ **115200**. |
| No boot banner appears | Open the monitor first, then tap **RST**. |
| `WiFi: still trying ...` repeats | Wrong SSID/password, or the hotspot isn't **2.4 GHz**. |
| `MQTT: connect failed rc=-2` | Broker not running, wrong `MQTT_HOST` IP, or ESP + Mac on different networks (§11). |
| `MQTT: connect failed rc=5` (not authorized) | Broker now requires credentials — set `MQTT_USER`/`MQTT_PASS`. |
| `rx` logged but no `CH:` line | Payload wasn't `on`/`off`/`toggle`, or topic didn't match a channel (see the `ignored:` line). |
| `reset : brownout` after boot | Power supply/USB port can't deliver enough current. |

**Adding logs:** use `logf("...", ...)` — never bare `Serial.print` — so new lines keep the
uptime prefix and consistent format.

## 10) Toolchain & Commands (PlatformIO)

PlatformIO CLI lives at `~/.platformio/penv/bin/pio` (invoke with the full path, or use
the VS Code PlatformIO buttons).

```bash
# build
~/.platformio/penv/bin/pio run

# build + flash the board
~/.platformio/penv/bin/pio run -t upload

# serial monitor (115200 baud — matches Serial.begin in setup())
~/.platformio/penv/bin/pio device monitor -b 115200

# build, flash, then open the monitor in one go
~/.platformio/penv/bin/pio run -t upload -t monitor

# list serial ports (find the ESP32)
~/.platformio/penv/bin/pio device list

# clean build artifacts
~/.platformio/penv/bin/pio run -t clean
```

`platformio.ini` pins the important bits:
- `board = 4d_systems_esp32s3_gen4_r8n16`
- `framework = arduino`
- `monitor_speed = 115200`
- `lib_deps = knolleary/PubSubClient@^2.8`

## 11) Common Task — Add a New Channel (e.g. a relay)

1. Wire the output and pick a **safe GPIO** (§4 — e.g. 15/16/17/18 for relays).
2. In `src/main.cpp`, add an entry to `channels[]`:
   ```cpp
   { "relay1", 15, false },
   ```
   `NUM_CH` and the `+/set` subscription update automatically.
3. Flash the board (§10). The device now accepts `home/dev1/relay1/set on|off|toggle`
   and reports `home/dev1/relay1/state`.
4. **Relays specifically:** drive through a **PC817 optocoupler**, confirm active-LOW vs
   active-HIGH polarity, and **dry-run with nothing on the switched side first**. If the
   board is active-LOW, invert the level in `applyChannel()` for that channel (or wire the
   opto to invert).
5. Verify over MQTT and in the serial log (§9, §12) before trusting it.

## 12) Broker & Verification

Start the broker (if not already running):
```bash
/opt/homebrew/sbin/mosquitto -c /opt/homebrew/etc/mosquitto/lan.conf -v
# check it's listening
lsof -nP -iTCP:1883 -sTCP:LISTEN
```

Round-trip verification checklist:
1. Serial log (§9) shows `WiFi: connected` then `MQTT: connected` + `subscribed`.
2. `mosquitto_sub -h 192.168.173.170 -t 'home/#' -v` shows `home/dev1/status online`.
3. Publish a `set`; confirm the physical output changes, the log shows `rx` → `CH:` → `tx`,
   and a matching `state` is echoed on the broker.
4. Power-cycle the device → `status` returns to `online` and all `state`s resync.

## 13) Flashing Gotcha (ESP32-S3)

The S3 often **won't auto-enter download mode**. If upload fails to find/sync the chip:

1. **Hold BOOT**, **tap RST**, **release BOOT** → puts it in download mode.
2. Flash (`pio run -t upload`).
3. **Tap RST** after flashing to run the app.

The USB serial port shows up as `/dev/cu.usbmodem*` and **its name changes in download
mode**, so re-check `pio device list` if the port disappears.

## 14) Safety Guardrails (Hard Rules)

- **Mains voltage is lethal.** Stage 5 only, after everything else works. Never wire mains
  while the board is powered or connected to USB.
- **Isolate mains from the ESP32** — optocouplers (PC817), remove relay VCC/JD-VCC jumper,
  enclose exposed conductors.
- **Dry-run relays** (click-test, nothing on the switched side) before connecting any load.
- A **relay is on/off only**. Fan **speed** control needs a **TRIAC / dimmer**, not a relay.
- **PZEM-004T** is the sanctioned mains-side power sensor (isolated). **No ACS712** on mains.
- Keep the broker **LAN-only**; anonymous access is fine on the hotspot but must never be
  internet-exposed.

## 15) Conventions

- **Firmware = C++ / PlatformIO.** Keep it single-file and readable until it genuinely needs
  splitting; match the existing comment density (each function has a short "why" comment).
- **All other components = JavaScript.**
- **One broker, one topic scheme** (§6). New capabilities are new channels/topics under
  `home/<device-id>/…`, not new transports.
- **Retained** for anything a late-joining client needs on connect (`state`, `status`).
- **Log through `logf()`**, not bare `Serial.print`, so every line keeps its uptime prefix
  and tag (§9).
- Never use a **reserved GPIO** (§4).
- Config that differs per board/network (Wi-Fi, broker IP, `DEVICE_ID`) lives in the config
  block at the top of `main.cpp`. (Move secrets out before pushing to git.)

## 16) Working With Claude on This Project

- The owner is **new to embedded** — explain wiring, pins, and MQTT concepts plainly; don't
  assume hardware background.
- **Respect the staged roadmap** (§3). Don't jump ahead to relays/mains before the prior
  stage is verified.
- **Safety first** (§14) — flag anything mains-related loudly and prefer the isolated,
  dry-run path.
- When firmware behavior changes, keep the **topic contract** (§6), the **log legend** (§9),
  and this guide in sync.
- Prefer verifying changes **end-to-end** — over MQTT and by reading the serial log
  (§9, §12), not just "it compiles."
