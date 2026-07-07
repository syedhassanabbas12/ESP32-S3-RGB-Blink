# ESP32-S3 Firmware — Setup & Flashing Guide

From a fresh machine to a provisioned ESP32-S3 controller talking to the broker.
Part of the HomeControl system; see the
[top-level bring-up guide](../../../HomeControl/SETUP.md) for the big picture.

---

## 1. What this is

Arduino/PlatformIO firmware for a **4D Systems ESP32-S3 (gen4, R8N16)** board. It
drives RGB LEDs + relays, connects to HiveMQ Cloud over MQTT/TLS, and speaks the
HomeControl topic contract (`home/<id>/<ch>/set|state`, telemetry, OTA, identify,
restart). No secrets are compiled in — broker host/credentials and the device id
are provisioned **at runtime** (BLE or serial) and saved to NVS. Details in
[`docs/features/`](docs/features/README.md).

---

## 2. Prerequisites

| Tool | Install |
|------|---------|
| VS Code | [code.visualstudio.com](https://code.visualstudio.com) |
| **PlatformIO IDE** extension | VS Code → Extensions → search "PlatformIO IDE" → Install |
| USB-C cable (data) | to connect the board |

PlatformIO installs its own toolchain/framework on first build — no separate
Arduino/ESP-IDF setup needed. (CLI alternative: `pip install platformio`, then use
`pio run` / `pio run -t upload`.)

Board config is already in [`platformio.ini`](platformio.ini):
```ini
[env:4d_systems_esp32s3_gen4_r8n16]
platform = espressif32
board    = 4d_systems_esp32s3_gen4_r8n16
framework = arduino
monitor_speed = 115200
board_build.partitions = partitions.csv     ; OTA-capable, preserves nvs + app0
lib_deps = knolleary/PubSubClient@^2.8
```

---

## 3. Build & flash

1. Open this folder in VS Code (or the shared `home-automation.code-workspace`).
2. Plug in the board. PlatformIO auto-detects the serial port
   (`/dev/cu.usbmodem*`).
3. PlatformIO toolbar (bottom bar): **✓ Build**, then **→ Upload** (flash).
   - CLI equivalent: `pio run` then `pio run -t upload`.
4. Open the serial monitor (**🔌 plug icon**, or `pio device monitor`) at **115200**
   to watch boot logs.

> **Flash gotcha:** if upload fails with *"port is busy / Resource temporarily
> unavailable"*, a serial monitor is holding the port. Close it (or kill
> `pio device monitor`), upload, then reopen the monitor.

---

## 4. Provision the device (first boot)

The firmware boots with empty config. Give it WiFi + broker details one of two ways:

### Option A — BLE provisioning (via the app)
On first boot (or after `prov`/`reset`) the device advertises BLE as
**`PROV_HomeControl`**. In the app: **Settings → Add / set up a device**, follow the
flow (proof-of-possession PoP = `homecontrol`). This sets WiFi. Set the broker +
device id over serial (Option B) if not already saved. See
[docs/features/ble-provisioning.md](docs/features/ble-provisioning.md).

### Option B — Serial console (115200)
Type these into the serial monitor:
```
wifi <SSID>|<PASSWORD>
set host=YOUR-CLUSTER.s1.eu.hivemq.cloud port=8883 user=YourUser pass=YourPass id=dev1
```
- `wifi <SSID>|<PASSWORD>` — saves WiFi to NVS (survives reboot).
- `set key=value ...` — space-separated pairs; saved to NVS. Keys: `host`, `port`
  (8883 for HiveMQ TLS), `user`, `pass`, `id`.
- `prov` — reboot into BLE WiFi provisioning.
- `reset` — wipe WiFi + broker settings, reboot into provisioning.

`id` **must match** the app's `DEVICE_ID` in `HomeControl/src/config.ts` (default
`dev1`), and the channels must match the app's `CHANNELS`.

After config, the device connects and publishes retained state — the app dashboard
shows it online.

---

## 5. Verify

Serial log should show `MQTT: connecting to <host>:8883 (TLS)` then a connect, and
`MQTT: tx home/dev1/<ch>/state = off` lines. In the app, toggling a channel should
flip the physical output and reflect back. If you run the automation server, its
`/status` will list `deviceOnline: { dev1: true }`.

---

## 6. Local Mosquitto broker (optional — LAN-only dev)

The default setup uses **HiveMQ Cloud** (works from anywhere). For offline/LAN
testing you can point the firmware + app at a local Mosquitto instead:

```sh
# start (not a brew service — it does NOT survive reboot):
/opt/homebrew/opt/mosquitto/sbin/mosquitto -c /opt/homebrew/etc/mosquitto/lan.conf
# check it's listening (1883 TCP for ESP32, 9001 websockets for the app):
lsof -nP -iTCP:1883 -iTCP:9001 | grep LISTEN
```
`lan.conf`: `listener 1883` + `listener 9001 protocol websockets` + `allow_anonymous true`.
Then set the firmware `host` to the Mac's LAN IP (`set host=<ip> port=1883`) and the
app's `BROKER_URL` accordingly. Remember the broker is frequently found stopped —
if the device/app "just can't connect," start it first.

---

## 7. OTA updates

Once running, new firmware is delivered over-the-air via GitHub Releases (this repo
is public so the download URL needs no auth). Bump `FW_VERSION`, build, attach the
`.bin` to a release, and trigger from the app. See
[docs/features/ota.md](docs/features/ota.md).

---

## 8. Pushing changes

```sh
TOK="$(cat /Users/hassanabbas/Documents/HomeControl/.ghtoken)"
git -c credential.helper= push \
  "https://x-access-token:${TOK}@github.com/syedhassanabbas12/ESP32-S3-RGB-Blink.git" \
  HEAD:refs/heads/main
```

---

## 9. Troubleshooting

| Symptom | Fix |
|---------|-----|
| Upload: "port is busy" | Close the serial monitor holding `/dev/cu.usbmodem*`, retry (§3) |
| Board not detected | Use a **data** USB-C cable; try another port; check `pio device list` |
| Won't connect to broker | Verify `set host/port/user/pass` (serial shows saved config); HiveMQ uses port **8883** TLS |
| Stuck / wrong WiFi | `reset` over serial, then re-provision |
| Wrong device in app | `id` must equal the app's `DEVICE_ID` |

## 10. More

- Firmware internals: [docs/features/README.md](docs/features/README.md)
- MQTT contract, provisioning, OTA, partitions, telemetry, channels/GPIO — each has its own doc.
- Project conventions: [CLAUDE.md](CLAUDE.md).
