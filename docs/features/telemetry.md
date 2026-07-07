# Device Telemetry (`home/<id>/telemetry`)

> **Status:** shipped · **Repo(s):** firmware (pairs with app) · **Since:** `src/main.cpp` (FW `2.6.0`)

Recreation-grade spec: someone (or Claude) with only this document should be able to
rebuild this feature identically. This is the **firmware half**. The app consumes this
payload in the device detail sheet — see `HomeControl/docs/features/device-detail-sheet.md`.

## 1. Summary
`publishTelemetry()` publishes a small **retained JSON** health snapshot to
`home/<id>/telemetry` so the app can show live diagnostics (firmware version, WiFi signal,
uptime, free heap) in the device detail sheet. It is published twice in the lifecycle:
once right after the MQTT connection is established (inside `connectMQTT()`), and then on a
**30 s heartbeat** from `loop()`. Because it is retained, the app sees the last-known
health immediately on subscribe, even if it connects between heartbeats.

## 2. User-facing behavior
There is no device UI. In the app's device detail sheet the user sees the four fields this
message carries:
- **Firmware version** (`fw`) — e.g. `2.6.0`, used by the OTA flow to decide if an update
  is available.
- **Signal** (`rssi`) — WiFi RSSI in dBm (negative; closer to 0 is stronger).
- **Uptime** (`uptime`) — seconds since boot; resets to 0 after a restart/OTA.
- **Free heap** (`heap`) — bytes of free RAM; a slow decline hints at a leak.

The values refresh at least every 30 s while the device is online, and appear
"instantly" on opening the sheet because the last snapshot is retained on the broker.

## 3. Architecture & files
| File | Role |
|------|------|
| `src/main.cpp` | `FW_VERSION` `#define`, `publishTelemetry()`, its call site in `connectMQTT()` (on connect), and the 30 s heartbeat in `loop()`. |
| `HomeControl/docs/features/device-detail-sheet.md` | App counterpart that parses this JSON and renders the four fields. |

How it plugs in: telemetry piggybacks on the existing `PubSubClient` MQTT connection
(`mqtt`) — no separate transport. It publishes nothing unless MQTT is connected.

## 4. Data model / types
The firmware version constant and the publisher (from `src/main.cpp`):

```cpp
#define FW_VERSION "2.6.0"

// Publish device health (firmware, signal, uptime, free heap) as retained JSON
// so the app can show live diagnostics in the device detail sheet.
// Topic: home/<id>/telemetry
void publishTelemetry() {
  if (!mqtt.connected()) return;
  char topic[80], payload[128];
  snprintf(topic, sizeof(topic), "home/%s/telemetry", DEVICE_ID);
  snprintf(payload, sizeof(payload),
           "{\"fw\":\"%s\",\"rssi\":%d,\"uptime\":%lu,\"heap\":%u}",
           FW_VERSION, WiFi.RSSI(), (unsigned long)(millis() / 1000), ESP.getFreeHeap());
  mqtt.publish(topic, payload, true);
}
```

Payload schema (JSON object, exactly these four keys, in this order):

| Key | JSON type | Source | Units / notes |
|-----|-----------|--------|---------------|
| `fw` | string | `FW_VERSION` `#define` | Semantic version, e.g. `"2.6.0"`. Quoted. |
| `rssi` | number (int) | `WiFi.RSSI()` | dBm, negative, e.g. `-58`. |
| `uptime` | number (unsigned) | `millis() / 1000` | Seconds since boot. |
| `heap` | number (unsigned) | `ESP.getFreeHeap()` | Bytes of free heap. |

Example payload:
```json
{"fw":"2.6.0","rssi":-58,"uptime":3720,"heap":214560}
```

`FW_VERSION` is a compile-time `#define`; bumping it is part of cutting a new firmware
release and is what the app's OTA flow compares against (see `ota.md` / `ota-app.md`).

## 5. Protocol / contract

| Property | Value |
|----------|-------|
| **Topic** | `home/<id>/telemetry` (`<id>` = configured `DEVICE_ID`, default `dev1`) |
| **Direction** | device → app |
| **Payload** | JSON object `{"fw":..,"rssi":..,"uptime":..,"heap":..}` (see §4) |
| **Retained** | **yes** (`mqtt.publish(topic, payload, true)`) — the broker keeps the last snapshot for new subscribers |
| **QoS** | 0 (PubSubClient publishes are QoS 0) |
| **Cadence** | once on MQTT connect, then every 30 s |

The app must subscribe to `home/<id>/telemetry` and parse the JSON. This topic is separate
from the control topics (`home/<id>/<ch>/set` / `/state`) and the `home/<id>/status`
online/offline Last-Will — see `mqtt-control.md` for the full topic map. The retained flag
is the contract that lets the detail sheet populate immediately instead of waiting up to
30 s for the next heartbeat.

## 6. Key logic

**Guard first.** `if (!mqtt.connected()) return;` — never attempt to publish without a live
connection; `publishTelemetry()` is safe to call speculatively.

**On-connect publish** (in `connectMQTT()`, right after subscribing and republishing all
channel states):
```cpp
      for (size_t i = 0; i < NUM_CH; i++) publishState(channels[i]);
      publishTelemetry();
```
Rationale: as soon as the app (or the retained state) is reachable, push a fresh health
snapshot so the sheet isn't showing a stale retained value from a previous session.

**30 s heartbeat** (in `loop()`), sharing the same `lastBeat` timer as the serial "alive"
log line:
```cpp
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 30000) {
    lastBeat = millis();
    logf("alive  uptime=%lus  heap=%u  rssi=%d dBm",
         millis() / 1000, ESP.getFreeHeap(), WiFi.RSSI());
    publishTelemetry();
  }
```
Rationale for **30 s**: frequent enough that the detail sheet's signal/uptime/heap feel
live, infrequent enough to be negligible traffic on the (metered) HiveMQ Cloud broker and
to avoid spamming retained messages. The heartbeat is coupled to the existing serial
liveness log so there is a single cadence to reason about.

**Buffer sizing:** `payload[128]` comfortably holds the four-field JSON (worst case ~60
chars); `topic[80]` matches the other topic buffers in the file. The MQTT client buffer is
set globally to 2048 in `setup()` (`mqtt.setBufferSize(2048)`), far larger than telemetry
needs — that size is driven by long OTA URLs, not this payload.

## 7. Dependencies & setup
- **`PubSubClient`** (`knolleary/PubSubClient@^2.8`, from `platformio.ini lib_deps`) —
  the `mqtt.publish(topic, payload, retained)` call.
- **`WiFi.h`** (Arduino-ESP32 core) — `WiFi.RSSI()`.
- **`esp32` runtime** — `millis()`, `ESP.getFreeHeap()`.
- No extra libraries; telemetry reuses the MQTT connection established for control.
- Requires `DEVICE_ID` and the broker to be configured (see `wifi-broker-config.md`) and
  the connection to be up (`connectMQTT()` in `mqtt-control.md`).

## 8. Edge cases & gotchas
- **Silent no-op when offline.** If MQTT isn't connected, `publishTelemetry()` returns
  immediately — the heartbeat still ticks but publishes nothing until reconnect.
- **QoS 0 + retained.** A heartbeat can be lost in transit (QoS 0), but the *retained*
  copy on the broker means the next successful publish (≤30 s later, or the on-connect one)
  restores an accurate snapshot. The app should treat telemetry as best-effort/latest-wins,
  not a guaranteed stream.
- **`uptime` resets on restart/OTA.** A sudden drop of `uptime` to a small number is the
  normal signal that the device rebooted (via `restart`/`identify`-adjacent commands or
  OTA). The app can use this to detect reboots.
- **`fw` is compile-time.** If you flash new firmware but forget to bump `FW_VERSION`, the
  app's OTA "update available" check (which compares this string) won't detect the change.
- **`rssi` is only valid while STA-connected.** Since publish only happens when MQTT (hence
  WiFi) is up, `WiFi.RSSI()` is always meaningful here.

## 9. Verification
- **Live broker:** subscribed to `home/dev1/telemetry` with an MQTT client and confirmed a
  retained message appears immediately on subscribe (proving `retained=true`), then a new
  payload arrives every ~30 s.
- **Payload shape:** validated the JSON parses and carries `fw`/`rssi`/`uptime`/`heap` with
  the expected types; confirmed `fw` == `2.6.0`.
- **On-connect publish:** power-cycled the device and confirmed a telemetry message is
  published right after `MQTT: connected` / channel-state republish, before the first 30 s
  heartbeat.
- **Reboot detection:** triggered a restart and confirmed `uptime` dropped back near 0 in
  the next telemetry payload.
- **App side:** confirmed the device detail sheet renders all four fields and refreshes on
  the 30 s cadence (`HomeControl/docs/features/device-detail-sheet.md`).

## 10. Related
- App counterpart: `HomeControl/docs/features/device-detail-sheet.md` (renders fw/rssi/
  uptime/heap, and hosts the Restart/Identify/OTA controls).
- `device-commands.md` — the `restart`/`identify` commands surfaced next to telemetry in
  the same sheet.
- `mqtt-control.md` — the full topic scheme, connect/subscribe, Last-Will.
- `ota.md` / `HomeControl/docs/features/ota-app.md` — how `fw` drives the update check.
