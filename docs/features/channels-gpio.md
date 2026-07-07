# Channels & GPIO Output Model

> **Status:** shipped · **Repo(s):** firmware · **Since:** firmware v2.6.0 (`src/main.cpp`)

Recreation-grade spec: someone (or Claude) with only this document should be able to
rebuild this feature identically. Prefer concrete detail over prose. Keep it current —
update this file in the same commit that changes the feature.

## 1. Summary
Every controllable output on the board — the three RGB LED channels and the four relay
channels — is described by one uniform `Channel` record. A single fixed-size `channels[]`
array is the firmware's entire device model: the MQTT layer, the boot init, and the
identify blink all iterate over it. Each channel carries its own GPIO pin and an
`activeLow` flag so that the on/off *intent* is decoupled from the electrical *level*
driven on the pin. This is what lets active-high LEDs and active-low relay boards live
in the same array and respond to the same `"on"`/`"off"` semantics. Everything is forced
to a known **OFF** state at boot before WiFi/MQTT ever come up, so the board never
energizes a relay or lights an LED by accident during startup.

## 2. User-facing behavior
There is no local UI on the board. The user interacts with these channels remotely (via
the app over MQTT — see `mqtt-control.md`):

- Sending `on` / `off` / `toggle` to a channel drives its GPIO and the physical output
  changes: an LED lights, or a relay clicks and its load switches.
- The three LED channels are named `red`, `green`, `blue`. The four relays are named
  `relay1`…`relay4`.
- At power-on / reset, **all seven outputs are OFF** — no LED lit, no relay energized —
  regardless of what they were doing before the reset. Retained state is re-published
  once MQTT connects (see §6).
- On an `identify` command the RGB LEDs blink together a few times, then return to
  whatever they were, so a user can physically locate one board among several.

## 3. Architecture & files
Every file involved and its role.

| File | Role |
|------|------|
| `src/main.cpp` | Defines `struct Channel`, the `channels[]` array, `NUM_CH`, `writePin()`, `applyChannel()`, the boot-time init loop in `setup()`, and `identifyBlink()`. This is the whole feature. |
| `platformio.ini` | Board `4d_systems_esp32s3_gen4_r8n16`, Arduino framework — provides `pinMode`/`digitalWrite`/`HIGH`/`LOW`. |

How it plugs in: `channels[]` is the shared source of truth. The MQTT control layer
(`mqtt-control.md`) resolves incoming `home/<id>/<name>/set` topics against
`channels[i].name`, calls `applyChannel()`, and mirrors state back on
`home/<id>/<name>/state`. `applyChannel()` is the *only* intended path to change an
output, because it keeps the in-memory `state`, the physical pin, and the retained MQTT
state in sync. (`identifyBlink()` is the one deliberate exception — it pokes the pins
directly and then republishes to re-sync, see §8.)

## 4. Data model / types
Full type definition and the array — pasted verbatim from `src/main.cpp`:

```cpp
struct Channel {
  const char *name;   // MQTT channel segment, e.g. "red" -> home/<id>/red/set
  uint8_t pin;        // ESP32-S3 GPIO number
  bool activeLow;     // true = pin LOW means "on" (relay board is active-low)
  bool state;         // logical on/off intent (NOT the pin level)
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
const size_t NUM_CH = sizeof(channels) / sizeof(channels[0]);   // == 7
```

**GPIO map** (ESP32-S3, board `4d_systems_esp32s3_gen4_r8n16`):

| Channel | GPIO | `activeLow` | Idle level (state=off) | Energized level (state=on) | Kind |
|---------|-----:|-------------|------------------------|----------------------------|------|
| `red`    | 4  | `false` | LOW  | HIGH | RGB LED (active-high) |
| `green`  | 5  | `false` | LOW  | HIGH | RGB LED (active-high) |
| `blue`   | 6  | `false` | LOW  | HIGH | RGB LED (active-high) |
| `relay1` | 15 | `true`  | HIGH | LOW  | Relay (active-low) |
| `relay2` | 16 | `true`  | HIGH | LOW  | Relay (active-low) |
| `relay3` | 17 | `true`  | HIGH | LOW  | Relay (active-low) |
| `relay4` | 18 | `true`  | HIGH | LOW  | Relay (active-low) |

`state` is the **logical** value, not the electrical one. The mapping from `state` to
pin level goes through `writePin()` (§6). The array literal initializes every
`state` to `false` (off), but boot re-asserts it anyway — see §6.

## 5. Protocol / contract
Channels are exposed over MQTT by name. For a device id `<id>` and channel `<name>`:

| Topic | Direction | Payload | Notes |
|-------|-----------|---------|-------|
| `home/<id>/<name>/set`   | app → device | `on` / `off` / `toggle` | Drives the channel via `applyChannel()`. |
| `home/<id>/<name>/state` | device → app | `on` / `off` (retained) | Published by `publishState()` after any change. |

The channel `<name>` is exactly the `Channel.name` string (`red`, `green`, `blue`,
`relay1`…`relay4`). Full topic table, QoS, retention and Last-Will are in
`mqtt-control.md`. The app-side model that consumes these is
[`mqtt-state-layer.md`](../../../../../HomeControl/docs/features/mqtt-state-layer.md).

## 6. Key logic

### `writePin()` — the active-low XOR
```cpp
void writePin(const Channel &c) {
  digitalWrite(c.pin, (c.state ^ c.activeLow) ? HIGH : LOW);
}
```
The whole active-low handling is one XOR. Truth table:

| `state` | `activeLow` | `state ^ activeLow` | Pin | Meaning |
|:-------:|:-----------:|:-------------------:|:---:|---------|
| off (0) | 0 (LED)   | 0 | LOW  | LED dark |
| on  (1) | 0 (LED)   | 1 | HIGH | LED lit |
| off (0) | 1 (relay) | 1 | HIGH | relay **de-energized** |
| on  (1) | 1 (relay) | 0 | LOW  | relay **energized** (coil pulled to LOW) |

So a relay energizes when its pin is driven **LOW**, exactly as an active-low relay
board expects. **Rationale:** encoding the polarity as a per-channel data flag (rather
than branching per channel, or wiring the polarity into every call site) means the LED
and relay code paths are identical — `applyChannel()`, `identifyBlink()`, and the boot
loop all call the same `writePin()` and never special-case relays. Adding a channel is a
one-line array edit.

### `applyChannel()` — the single mutation path
```cpp
void applyChannel(Channel &c, bool on) {
  c.state = on;
  writePin(c);
  logf("CH:   %s (gpio %u) -> %s", c.name, c.pin, on ? "ON" : "off");
  publishState(c);
}
```
Order matters and is deliberate: (1) update the logical `state`, (2) drive the pin from
that state, (3) log, (4) publish the retained `state` topic so the app reflects reality.
Because publish reads `c.state` (set in step 1), the app is always told the truth even if
`writePin` is a no-op. `applyChannel()` is called from `onMessage()` for `on`/`off`/`toggle`
(`toggle` passes `!channels[i].state`).

### Boot: everything OFF before anything else
The very first thing `setup()` does after logging chip info — **before** `loadConfig()`,
before TLS, before WiFi/BLE — is force every channel off:
```cpp
for (size_t i = 0; i < NUM_CH; i++) {
  pinMode(channels[i].pin, OUTPUT);
  channels[i].state = false;
  writePin(channels[i]);
  logf("CH:   %s on gpio %u -> off%s", channels[i].name, channels[i].pin,
       channels[i].activeLow ? " (active-low)" : "");
}
```
For each channel: set the pin to `OUTPUT`, set logical `state = false`, then `writePin()`.
Via the XOR, LEDs go LOW (dark) and relays go HIGH (de-energized). **Rationale:** ESP32
GPIOs float / can glitch during reset; explicitly asserting the de-energized level as the
first action guarantees no relay clicks on and no LED flashes on power-up, independent of
prior state (state is not persisted — see §8). This runs before config load so it is
unconditional and cannot be skipped by a bad/empty NVS.

### `identifyBlink()` — locate the board
```cpp
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
Operates on indices 0–2 (the RGB channels), snapshots their state, blinks all three
together 3 times (6 half-cycles × 160 ms ≈ 1 s), then restores and **republishes** the
saved state. It writes pins directly (bypassing `applyChannel`) to avoid publishing 12
transient state messages during the blink; it re-syncs the app only at the end.
**Rationale for the ~1 s blocking delay:** identify is a rare, on-demand action; a short
blocking loop is simpler and safe here, versus a non-blocking timer state machine that
would add complexity to `loop()` for no user benefit.

## 7. Dependencies & setup
- **Framework:** Arduino for ESP32 (`framework = arduino`) — supplies `pinMode`,
  `digitalWrite`, `HIGH`, `LOW`, `delay`, `millis`.
- **Board:** `4d_systems_esp32s3_gen4_r8n16` (`platformio.ini`). GPIO numbers 4/5/6 and
  15/16/17/18 are valid ESP32-S3 output-capable pins.
- No external library is needed for the channel model itself; `PubSubClient` (see
  `platformio.ini` `lib_deps`) is only used by the publish side.
- Entry wiring: `channels[]`, `NUM_CH`, `writePin`, `applyChannel`, `identifyBlink` are
  all in `src/main.cpp`; the boot loop is inside `setup()`.

## 8. Edge cases & gotchas
- **State is not persisted across reboots.** There is deliberately no NVS save of
  channel state. A power cycle or an MQTT/OTA-triggered restart returns every output to
  OFF. The app's view is restored from the **retained** `.../state` topics that the
  device re-publishes on MQTT connect (`connectMQTT()` calls `publishState()` for all
  channels), not from device flash. If the broker had no retained state, the app sees
  the fresh boot-time `off`.
- **`activeLow` inverts idle level, so a relay pin idles HIGH.** If you probe a relay
  GPIO at rest expecting LOW, you'll be surprised — HIGH is correct (de-energized).
- **`name` collides with topic parsing.** The channel name is used verbatim as the MQTT
  topic segment and matched with `==`. Names must be unique and must not contain `/` or a
  space (the RGB/relay names are safe).
- **`identifyBlink()` bypasses `applyChannel()`.** It is the only place pins are driven
  without going through the single mutation path; it is careful to snapshot and restore
  `state` and republish, but any *incoming* set for red/green/blue during the ~1 s blink
  is processed after the blink returns (single-threaded loop) and will overwrite the
  restore — acceptable given how rare identify is.
- **Adding a channel:** add one row to `channels[]`; `NUM_CH`, the boot loop, the MQTT
  subscribe wildcard (`home/<id>/+/set`), and `publishState` all scale automatically. No
  other edits.
- **Pin conflicts:** GPIOs are hard-coded; changing wiring means editing the array's
  `pin`/`activeLow` fields, not any logic.

## 9. Verification
- **On-device serial log:** at boot the init loop prints one `CH:` line per channel, e.g.
  `CH:   relay1 on gpio 15 -> off (active-low)`, confirming pin, order, and polarity.
- **Physical check:** with a meter, LED pins (4/5/6) read LOW at rest and HIGH when set
  `on`; relay pins (15/16/17/18) read HIGH at rest and LOW when set `on` (relay clicks).
- **MQTT round-trip:** publishing `on` to `home/<id>/red/set` produces
  `CH:   red (gpio 4) -> ON` in the log and a retained `home/<id>/red/state = on`
  (`MQTT: tx` line). Toggling flips it.
- **Identify:** publishing to `home/<id>/identify` blinks R/G/B ~3× and the pre-blink
  state is restored (verified by the trailing `MQTT: tx` state republishes).

## 10. Related
- [`mqtt-control.md`](mqtt-control.md) — how `set`/`state` topics drive and mirror these channels.
- [`device-commands.md`](device-commands.md) — `identify` (uses `identifyBlink()`) and `restart`.
- [`wifi-broker-config.md`](wifi-broker-config.md) — device id `<id>` used in the topic names.
- App: [`mqtt-state-layer.md`](../../../../../HomeControl/docs/features/mqtt-state-layer.md) — consumes `state`, sends `set`.
- App: [`device-tile.md`](../../../../../HomeControl/docs/features/device-tile.md) — the toggle UI a channel maps to.
