# Remote Device Commands (`restart` + `identify`)

> **Status:** shipped · **Repo(s):** firmware (pairs with app) · **Since:** `src/main.cpp`

Recreation-grade spec: someone (or Claude) with only this document should be able to
rebuild this feature identically. This is the **firmware half**. The app triggers these
from the device detail sheet — see `HomeControl/docs/features/device-detail-sheet.md`.

## 1. Summary
Two fire-and-forget remote commands let the app act on a specific device over MQTT,
independent of the channel on/off controls. `home/<id>/restart` reboots the device (after
politely publishing `offline` to its status topic). `home/<id>/identify` flashes the three
RGB LEDs for about a second so a human can physically locate the board, then restores the
LEDs to exactly their prior state and republishes it so the app stays consistent. Both
topics are subscribed at MQTT-connect time and dispatched in the `onMessage` callback; both
ignore their payload (send an empty payload).

## 2. User-facing behavior
In the app's device detail sheet there are **Restart** and **Identify** buttons.
- **Restart:** the user taps Restart → the device's status goes to `offline` (the app can
  show it dropping) → the board reboots → it comes back `online` a few seconds later, WiFi
  reconnects, MQTT reconnects, and telemetry/uptime reset (uptime near 0). Serial logs:
  `CMD: restart requested over MQTT — rebooting`.
- **Identify:** the user taps Identify → the physical RGB LEDs on the target board blink
  ~1 s (3 on/off cycles) → they return to whatever they were before (off stays off, on
  stays on). The app's toggle states do not visibly change because the firmware republishes
  the original state at the end. Serial logs:
  `CMD: identify — blinking to locate device`.

## 3. Architecture & files
| File | Role |
|------|------|
| `src/main.cpp` | Subscriptions to `home/<id>/restart` and `home/<id>/identify` in `connectMQTT()`; the dispatch in `onMessage()`; `identifyBlink()`; the channel model + `writePin()`/`publishState()` reused by identify. |
| `HomeControl/docs/features/device-detail-sheet.md` | App counterpart with the Restart / Identify buttons that publish these topics. |

How it plugs in: both commands ride the same `PubSubClient` connection and callback used
for channel control. `identify` reuses the `Channel` model and `writePin()`/`publishState()`
helpers documented in `channels-gpio.md`.

## 4. Data model / types
`identify` operates on the first three channels, which are the RGB LEDs, from the global
channel table (from `src/main.cpp`):

```cpp
struct Channel {
  const char *name;
  uint8_t pin;
  bool activeLow;
  bool state;
};

Channel channels[] = {
  { "red",    4,  false, false },
  { "green",  5,  false, false },
  { "blue",   6,  false, false },
  { "relay1", 15, true,  false },
  { "relay2", 16, true,  false },
  { "relay3", 17, true,  false },
  { "relay4", 18, true,  false },
};
```
`channels[0..2]` (`red`/`green`/`blue`) are the RGB LEDs that `identifyBlink()` flashes.
`restart` holds no persistent state — it only touches `home/<id>/status`.

## 5. Protocol / contract

| Command | Topic | Direction | Payload | Retained | QoS |
|---------|-------|-----------|---------|----------|-----|
| Restart | `home/<id>/restart` | app → device | **empty** (ignored) | no | 0 |
| Identify | `home/<id>/identify` | app → device | **empty** (ignored) | no | 0 |

Side-effects published by the firmware:
- **Restart** publishes `offline` (retained) to `home/<id>/status` before rebooting; on
  reconnect `connectMQTT()` republishes `online` (retained) — this is the same
  Last-Will-backed status topic documented in `mqtt-control.md`.
- **Identify** republishes each RGB channel's restored state to `home/<id>/<ch>/state`
  (retained) after blinking, so the app's toggle states remain accurate.

Neither command reads its payload, so the app should publish an empty string. Both are
QoS 0 and non-retained on the command topic itself — a retained command would re-fire on
every reconnect, which is exactly what you do **not** want for "reboot" or "blink."

## 6. Key logic

### 6.1 Subscriptions (in `connectMQTT()`)
Registered once, immediately after a successful connect, alongside the channel-set
wildcard subscription:
```cpp
      char sub[80];
      snprintf(sub, sizeof(sub), "home/%s/+/set", DEVICE_ID);
      mqtt.subscribe(sub);
      logf("MQTT: subscribed to %s", sub);
      char restartTopic[80];
      snprintf(restartTopic, sizeof(restartTopic), "home/%s/restart", DEVICE_ID);
      mqtt.subscribe(restartTopic);
      char identifyTopic[80];
      snprintf(identifyTopic, sizeof(identifyTopic), "home/%s/identify", DEVICE_ID);
      mqtt.subscribe(identifyTopic);
```
`restart` and `identify` are subscribed **explicitly** (not caught by the `home/<id>/+/set`
wildcard) because they don't end in `/set` — they are single-segment command topics.

### 6.2 Dispatch (in `onMessage()`)
The callback trims the payload, logs the raw message, then matches the topic. `identify`
and `restart` are checked before the channel loop:
```cpp
  if (t == String("home/") + DEVICE_ID + "/identify") {
    logf("CMD:  identify — blinking to locate device");
    identifyBlink();
    return;
  }
  if (t == String("home/") + DEVICE_ID + "/restart") {
    logf("CMD:  restart requested over MQTT — rebooting");
    char statusTopic[80];
    snprintf(statusTopic, sizeof(statusTopic), "home/%s/status", DEVICE_ID);
    mqtt.publish(statusTopic, "offline", true); // tell the app before we go
    delay(300);
    ESP.restart();
  }
```
**Restart rationale:** publish `offline` (retained) *before* `ESP.restart()` so the app
sees an orderly departure instead of waiting for the broker's Last-Will keepalive timeout.
The `delay(300)` gives the TLS/MQTT stack time to actually flush that publish over the wire
before the CPU resets — without it the message can be dropped mid-send. `ESP.restart()`
never returns; the device reboots and `connectMQTT()` republishes `online` on reconnect.

### 6.3 `identifyBlink()`
```cpp
// Blink the RGB LEDs a few times to help locate this device, then restore their
// previous state. Briefly blocking (~1s) — fine for an on-demand action.
void identifyBlink() {
  bool prev[3] = { channels[0].state, channels[1].state, channels[2].state };
  for (int i = 0; i < 6; i++) {
    bool on = (i % 2) == 0;
    for (int ch = 0; ch < 3; ch++) {
      channels[ch].state = on;
      writePin(channels[ch]);
    }
    delay(160);
  }
  for (int ch = 0; ch < 3; ch++) {
    channels[ch].state = prev[ch];
    writePin(channels[ch]);
    publishState(channels[ch]); // restore + keep the app consistent
  }
}
```
Behavior and rationale:
- **Snapshot first:** capture `prev[3]` = the current RGB states so they can be restored
  exactly — identify must be side-effect-free from the user's perspective.
- **6 iterations × 160 ms ≈ 960 ms (~1 s):** `i%2` alternates all three LEDs on/off
  together, giving 3 visible flashes. The ~1 s duration is long enough to spot across a
  room, short enough that the blocking `delay()` doesn't starve MQTT keepalive.
- **Blocking is acceptable here** because identify is a rare, explicit, on-demand action;
  it writes GPIO directly via `writePin()` rather than going through `applyChannel()`
  (which would publish state on every flash and spam the app).
- **Restore + republish:** set each LED back to `prev[ch]`, drive the pin, and
  `publishState()` (retained) so the app's per-channel toggle reflects the true post-blink
  state — critical if a race made the app think an LED changed.

`writePin()` handles the active-low logic per channel (`digitalWrite(c.pin,
(c.state ^ c.activeLow) ? HIGH : LOW)`); the RGB channels are active-high, so this is a
straightforward drive, but reusing `writePin()` keeps identify correct regardless.

## 7. Dependencies & setup
- **`PubSubClient`** (`knolleary/PubSubClient@^2.8`) — `subscribe()`, `publish()`, and the
  `onMessage` callback registered via `mqtt.setCallback(onMessage)` in `setup()`.
- **`esp32` runtime** — `ESP.restart()`, `delay()`, `digitalWrite()`.
- Reuses the channel model + `writePin()`/`publishState()` from `channels-gpio.md`.
- Requires a live MQTT connection (`connectMQTT()`, `mqtt-control.md`) and a configured
  `DEVICE_ID`/broker (`wifi-broker-config.md`).
- No additional libraries beyond what channel control already needs.

## 8. Edge cases & gotchas
- **Command topics must NOT be retained.** If the app published `restart`/`identify` with
  the retained flag, the broker would re-deliver on every device reconnect — causing a
  reboot loop or a phantom blink on each connect. Publish these non-retained.
- **Empty payload by design.** Both handlers ignore the payload; the app sends `""`. Any
  payload is accepted but discarded.
- **`restart` needs the `delay(300)`.** Removing it risks `ESP.restart()` cutting off the
  `offline` publish before it leaves the TLS buffer, so the app would only learn the device
  is gone via the slower Last-Will timeout.
- **`identify` is blocking (~1 s).** During the blink, `mqtt.loop()` is not serviced.
  Because it's ~1 s and MQTT keepalive is far longer, this is safe — but do not lengthen the
  blink significantly without moving it off the callback thread.
- **Identify preserves and re-announces state.** The final `publishState()` per channel is
  what keeps the app's toggles truthful; dropping it would leave the app potentially
  showing the wrong LED state after an identify.
- **Dispatch order:** `identify`/`restart` are matched before the channel `/set` loop in
  `onMessage`, and each returns early, so they can never be mistaken for a channel command.

## 9. Verification
- **Live broker — restart:** published an empty message to `home/dev1/restart`; observed
  `home/dev1/status` flip to `offline` immediately, the device reboot, then `online`
  republished on reconnect and telemetry `uptime` reset near 0. Serial showed
  `CMD: restart requested over MQTT — rebooting`.
- **Live broker — identify:** with the RGB LEDs off, published empty to
  `home/dev1/identify`; watched the LEDs blink ~1 s (3 flashes) and return to off. Repeated
  with one LED on and confirmed it was restored to on and `home/dev1/<ch>/state`
  republished. Serial showed `CMD: identify — blinking to locate device`.
- **Retained-safety check:** confirmed the app publishes both commands non-retained (no
  reboot/blink loop on device reconnect).
- **App side:** confirmed the detail-sheet Restart and Identify buttons drive the above
  (`HomeControl/docs/features/device-detail-sheet.md`).

## 10. Related
- App counterpart: `HomeControl/docs/features/device-detail-sheet.md` (Restart / Identify
  buttons, plus telemetry rendering).
- `telemetry.md` — `uptime` resetting is how a restart is observed; both live in the same
  sheet.
- `mqtt-control.md` — the `home/<id>/status` online/offline topic + Last-Will that
  `restart` coordinates with, and the connect/subscribe flow.
- `channels-gpio.md` — the `Channel` model, `writePin()`, active-low handling, and
  `publishState()` reused by `identifyBlink()`.
