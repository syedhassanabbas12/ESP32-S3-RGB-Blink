# BLE WiFi Provisioning (firmware)

> **Status:** shipped · **Repo(s):** firmware (pairs with app) · **Since:** `src/main.cpp`

Recreation-grade spec: someone (or Claude) with only this document should be able to
rebuild this feature identically. This is the **firmware half**. The app half is
documented in `HomeControl/docs/features/ble-provisioning-app.md` — read both to
understand the full handshake.

## 1. Summary
The device has **no WiFi credentials compiled in**. On first boot (or whenever saved
WiFi can't connect) it starts Espressif's official BLE provisioning and advertises as a
Bluetooth LE peripheral named `PROV_HomeControl`. The companion app scans for the
`PROV_` prefix, connects, completes a secure (Security 1) handshake using a shared
proof-of-possession (`homecontrol`), then hands over the SSID/password. The ESP32 saves
those credentials to its own WiFi NVS, connects to STA, and — because the scheme handler
is `FREE_BTDM` — releases the entire Bluetooth stack to reclaim RAM for TLS/MQTT. Because
Bluetooth is freed, re-provisioning is not live: it always requires a reboot back into
provisioning mode. Three things can request that reboot: the serial `prov` command, the
serial `reset` command (which also wipes settings), and an automatic fallback that fires
if saved WiFi fails to connect within ~90 s of boot.

## 2. User-facing behavior
There is no screen on the device; behavior is observed over the USB serial monitor
(115200 baud) and via the app.

- **First boot (no saved WiFi):** the device advertises over BLE as `PROV_HomeControl`.
  Serial prints:
  `PROV: BLE provisioning started — open the app, device "PROV_HomeControl", PoP "homecontrol"`.
- **In the app:** the user opens the provisioning screen, the app finds `PROV_HomeControl`,
  the user enters the proof-of-possession `homecontrol`, picks a network, and types its
  password. Firmware logs each step: `PROV: received WiFi credentials for SSID "..."`,
  then `PROV: WiFi credentials accepted`, then `WiFi: connected ip=... rssi=... mac=...`,
  then `PROV: provisioning finished (BLE released)`.
- **Wrong WiFi password:** `PROV: credentials FAILED (wrong WiFi password or AP unreachable)`
  and the app can retry over the still-open BLE link.
- **Re-provision on demand:** the user sends `prov` over serial → the device reboots and
  comes back advertising `PROV_HomeControl` again.
- **Factory-ish reset:** the user sends `reset` over serial → WiFi + broker settings are
  wiped and the device reboots straight into provisioning.
- **Silent recovery:** if the saved network is gone/changed, after ~90 s the device
  reboots itself into provisioning without any user action, so the app can reach it over
  BLE and hand over new credentials.
- **Serial recovery (dev/no-app):** `wifi <SSID>|<PASSWORD>` sets credentials directly
  over USB without using BLE at all.

## 3. Architecture & files
| File | Role |
|------|------|
| `src/main.cpp` | Everything below: `PROV_SERVICE_NAME`/`PROV_POP` constants, `onProvEvent()` handler, `startProvisioning()`, `setForceProv()`/`takeForceProv()` NVS flag, the `prov`/`reset`/`wifi` serial commands in `handleSerialConfig()`, the setup wiring, and the 90 s auto-fallback in `loop()`. |
| `platformio.ini` | Pulls in the Arduino-ESP32 framework which provides `WiFiProv` (`WiFiProv.h`). No extra lib is needed for provisioning. |
| `partitions.csv` | Keeps the `nvs` partition across OTA so saved WiFi/broker config and the `forceprov` flag survive firmware migration (see `partitions.md`). |
| `HomeControl/docs/features/ble-provisioning-app.md` | The counterpart app doc (BLE scan, PoP entry, credential push). |

How it plugs in: `WiFi.onEvent(onProvEvent)` is registered in `setup()` before
`startProvisioning()` is called, so the provisioning + STA lifecycle is driven entirely
through Arduino WiFi events. The rest of the firmware (MQTT/TLS) only proceeds once
`WiFi.status() == WL_CONNECTED` in `loop()`.

## 4. Data model / types
Provisioning identity constants and the runtime state flags (from `src/main.cpp`):

```cpp
// BLE provisioning identity. The app scans for the "PROV_" prefix and must
// supply this proof-of-possession to complete the secure (Security 1) handshake.
const char *PROV_SERVICE_NAME = "PROV_HomeControl";
const char *PROV_POP          = "homecontrol";

volatile bool g_provisioning  = false;   // true while BLE provisioning is active
bool          g_everConnected = false;   // have we reached STA-connected since boot?
uint32_t      g_bootAt        = 0;
```

**Storage keys.** Two separate NVS namespaces are in play:

| Namespace | Key | Type | Meaning |
|-----------|-----|------|---------|
| `homecfg` (app's `Preferences`) | `forceprov` | bool | One-shot "reboot into BLE provisioning" flag. Set before a restart, consumed once on boot. |
| WiFi driver NVS (managed by the ESP-IDF WiFi stack, not `Preferences`) | — | — | The SSID/password saved by provisioning or by `WiFi.persistent(true)`. Erased by `WiFi.disconnect(true, true)`. |

The `forceprov` flag lives in the same `homecfg` namespace as the MQTT broker config
(`host`, `port`, `user`, `pass`, `devid`), so `prefs.clear()` on `reset` wipes it too —
but `reset` then re-sets it before rebooting.

## 5. Protocol / contract
Provisioning uses **BLE GATT**, not MQTT — it is the Espressif `wifi_prov_mgr` protocol
(protocomm over BLE), transported with `WIFI_PROV_SCHEME_BLE`. The firmware↔app contract
is three shared values that MUST match on both sides:

| Contract value | Firmware | App (`ble-provisioning-app.md`) |
|----------------|----------|-------------------------------|
| Advertised service name | `PROV_HomeControl` (app scans for the `PROV_` prefix) | must scan for / connect to `PROV_HomeControl` |
| Proof-of-possession (PoP) | `homecontrol` | must send `homecontrol` |
| Security version | `WIFI_PROV_SECURITY_1` (X25519 + AES-CTR, PoP-authenticated) | must use Security 1 |

If any of the three differ, the Security 1 handshake fails and no credentials are
exchanged. There is **no MQTT** involved until after WiFi connects; the MQTT topic
contract (`home/<id>/status`, etc.) is documented in `mqtt-control.md`.

## 6. Key logic

### 6.1 Starting provisioning
```cpp
// Start BLE provisioning. reset=true forces it even if WiFi was already saved.
void startProvisioning(bool reset) {
  logf("PROV: begin (force=%s)", reset ? "yes" : "no");
  // FREE_BTDM frees the Bluetooth stack once provisioning ends, reclaiming RAM
  // for TLS/MQTT during normal operation.
  WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BTDM,
                          WIFI_PROV_SECURITY_1, PROV_POP, PROV_SERVICE_NAME,
                          NULL, NULL, reset);
}
```
Argument-by-argument:
- `WIFI_PROV_SCHEME_BLE` — provision over Bluetooth LE (vs SoftAP). BLE is chosen because
  the app can drive it with no manual WiFi-network switching on the phone.
- `WIFI_PROV_SCHEME_HANDLER_FREE_BTDM` — **the rationale for this whole design.** Once
  provisioning ends, the BLE + Bluetooth-Dual-Mode controller memory is freed and returned
  to the heap. This reclaims the RAM the device needs for the TLS socket + MQTT during
  normal operation. The trade-off: Bluetooth is gone until the next reboot, so
  **re-provisioning is not possible live — it always requires a reboot** back into this
  function. Rejected alternative: `WIFI_PROV_SCHEME_HANDLER_FREE_BLE` (frees only BLE) or
  keeping the stack resident — both leave less heap for TLS. The reboot-to-reprovision cost
  is acceptable because re-provisioning is rare.
- `WIFI_PROV_SECURITY_1` — the encrypted, PoP-authenticated handshake (see §5).
- `PROV_POP` / `PROV_SERVICE_NAME` — the shared contract values.
- `NULL, NULL` — no `service_key` (BLE ignores it) and no custom `uuid`.
- `reset` — the last arg. When `true`, provisioning is forced even if credentials are
  already stored (this is how `prov`/`reset`/auto-fallback re-enter provisioning). When
  `false`, `beginProvision` connects with saved credentials if present and only advertises
  over BLE if there are none (the normal-boot path).

### 6.2 The provisioning event handler
`onProvEvent` is registered via `WiFi.onEvent(onProvEvent)` and receives both PROV_* and
WIFI_* events. It maintains the two state flags used by the auto-fallback:

```cpp
void onProvEvent(arduino_event_t *e) {
  switch (e->event_id) {
    case ARDUINO_EVENT_PROV_START:
      g_provisioning = true;
      logf("PROV: BLE provisioning started — open the app, device \"%s\", PoP \"%s\"",
           PROV_SERVICE_NAME, PROV_POP);
      break;
    case ARDUINO_EVENT_PROV_CRED_RECV:
      logf("PROV: received WiFi credentials for SSID \"%s\"",
           (const char *)e->event_info.prov_cred_recv.ssid);
      break;
    case ARDUINO_EVENT_PROV_CRED_FAIL:
      logf("PROV: credentials FAILED (wrong WiFi password or AP unreachable)");
      break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      logf("PROV: WiFi credentials accepted");
      break;
    case ARDUINO_EVENT_PROV_END:
      g_provisioning = false;
      logf("PROV: provisioning finished (BLE released)");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      g_everConnected = true;
      logf("WiFi: connected  ip=%s  rssi=%d dBm  mac=%s",
           WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.macAddress().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      // ESP32 auto-reconnects; only log to avoid noise.
      break;
    default: break;
  }
}
```
`g_provisioning` gates the auto-fallback (never reboot while the user is mid-handshake).
`g_everConnected` records whether STA ever reached GOT_IP this boot — the auto-fallback
only fires if we have *never* connected, so a device that connected and later dropped just
lets the ESP32 auto-reconnect (`WiFi.setAutoReconnect(true)`) instead of rebooting.

### 6.3 The "force provisioning" NVS flag
Because BLE is freed after provisioning, the only way back in is a reboot. A persisted
one-shot flag carries the intent across `ESP.restart()`:

```cpp
// One-shot "reboot into BLE provisioning" flag (survives the restart).
void setForceProv() { prefs.begin("homecfg", false); prefs.putBool("forceprov", true); prefs.end(); }
bool takeForceProv() {
  prefs.begin("homecfg", false);
  bool f = prefs.getBool("forceprov", false);
  if (f) prefs.putBool("forceprov", false);   // clear so it only fires once
  prefs.end();
  return f;
}
```
`takeForceProv()` is *consuming* — it reads and immediately clears, so the flag fires
exactly once and the device doesn't get stuck in a provisioning loop.

### 6.4 setup() wiring
```cpp
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onProvEvent);
  bool force = takeForceProv();
  if (force) logf("PROV: force flag set — entering BLE provisioning");
  startProvisioning(force);
```
On every boot: read+clear `forceprov`, then call `startProvisioning(force)`. If the flag
was set (`prov`/`reset`/auto-fallback), `force==true` and BLE advertising starts
unconditionally. Otherwise `force==false` and `beginProvision` silently connects with
saved credentials, only advertising if there are none.

### 6.5 Serial `prov` / `reset` (in `handleSerialConfig()`)
```cpp
  if (line == "prov") {
    logf("CMD:  rebooting into BLE provisioning");
    setForceProv();
    delay(300); ESP.restart();
  }
  if (line == "reset") {
    logf("CMD:  wiping WiFi + broker settings, rebooting into provisioning");
    prefs.begin("homecfg", false); prefs.clear(); prefs.end();
    WiFi.disconnect(true, true);   // erase stored WiFi credentials
    setForceProv();
    delay(500); ESP.restart();
  }
```
- `prov` — set the flag, reboot. Keeps all saved settings; just re-enters provisioning.
- `reset` — clears the whole `homecfg` namespace (broker + `forceprov`), erases WiFi creds
  from the driver NVS via `WiFi.disconnect(true, true)`, **then** re-sets `forceprov` (so
  the flag survives the `clear()`), then reboots. The device comes up with no config and
  goes straight to provisioning.

### 6.6 The 90 s auto-fallback (in `loop()`)
```cpp
  if (WiFi.status() != WL_CONNECTED) {
    // If we're actively provisioning over BLE, just wait for the user/app.
    // Otherwise, if we had saved WiFi but never connected this boot (e.g. the
    // network moved/changed), reboot into BLE provisioning after a grace period
    // so the app can reach us over Bluetooth and hand over new credentials.
    if (!g_provisioning && !g_everConnected && millis() - g_bootAt > 90000) {
      logf("WiFi: no connection in 90s — rebooting into BLE provisioning");
      setForceProv();
      delay(500); ESP.restart();
    }
    return;
  }
```
Three guards must all hold before the reboot fires: **not** currently provisioning (don't
interrupt a live handshake), **never** connected this boot (a device that connected once
and dropped should auto-reconnect, not reboot), and **≥90 s** since boot
(`g_bootAt = millis()` is captured at the top of `setup()`). The 90 s window is a
deliberate compromise: long enough for a slow AP / DHCP / weak-signal join, short enough
that an unrecoverable device self-heals into provisioning within a couple of minutes
instead of sitting dark.

### 6.7 Serial WiFi recovery (`wifi <SSID>|<PASSWORD>`)
```cpp
  if (line.startsWith("wifi ")) {
    String rest = line.substring(5);
    int bar = rest.indexOf('|');
    String ssid = bar >= 0 ? rest.substring(0, bar) : rest;
    String pass = bar >= 0 ? rest.substring(bar + 1) : "";
    logf("CMD:  setting WiFi over serial, ssid=\"%s\"", ssid.c_str());
    WiFi.persistent(true);         // save creds to NVS (survives reboot)
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    return false;
  }
```
A bench/dev path that bypasses BLE entirely. The `|` separator is used instead of a space
so SSIDs and passwords that contain spaces still parse correctly. `WiFi.persistent(true)`
writes the credentials to the driver NVS so they survive the next reboot exactly like
BLE-provisioned creds.

## 7. Dependencies & setup
- **`WiFiProv.h`** — part of the Arduino-ESP32 core (no PlatformIO `lib_deps` entry
  needed). Provides `WiFiProv.beginProvision(...)` and the `ARDUINO_EVENT_PROV_*` events.
- **`WiFi.h`** — Arduino-ESP32 core; STA mode, events, `setAutoReconnect`, `persistent`.
- **`Preferences.h`** — Arduino-ESP32 core; the `homecfg` NVS namespace holding `forceprov`.
- **Board / framework** (`platformio.ini`): `board = 4d_systems_esp32s3_gen4_r8n16`,
  `framework = arduino`, `platform = espressif32`. The ESP32-S3 has the BLE radio required
  for `WIFI_PROV_SCHEME_BLE`.
- **Partitions** (`board_build.partitions = partitions.csv`): the `nvs` partition is
  preserved across OTA, so provisioned WiFi + the `forceprov` flag survive firmware
  migration.
- **Bluetooth RAM budget:** `FREE_BTDM` is what makes the 2048-byte MQTT buffer + TLS fit
  after provisioning. Do not switch to a handler that keeps BT resident without re-checking
  free heap.

## 8. Edge cases & gotchas
- **Re-provisioning is never live.** Because `FREE_BTDM` frees the Bluetooth controller,
  you cannot re-provision a running device over BLE — you must reboot into provisioning
  (`prov`, `reset`, or the auto-fallback). This is by design, not a bug.
- **`reset` order matters.** `prefs.clear()` also erases `forceprov`, so `setForceProv()`
  is called *after* the clear. Reordering them would boot the device into a
  no-config-but-no-provisioning limbo.
- **`forceprov` is consumed once.** `takeForceProv()` clears the flag as it reads it; a
  crash-loop won't get stuck re-provisioning forever.
- **Auto-fallback won't fire on a device that ever connected.** `g_everConnected` latches
  `true` on the first `GOT_IP` and never resets, so a mid-session WiFi drop relies on the
  ESP32's own auto-reconnect (`WiFi.setAutoReconnect(true)`), not a reboot.
- **PoP/name must match the app exactly.** `homecontrol` and `PROV_HomeControl` are the
  contract; a mismatch fails the Security 1 handshake with no useful device-side error
  beyond `CRED_FAIL`/timeout. Change both repos together.
- **90 s is measured from boot,** not from WiFi start; the two are effectively the same
  since `startProvisioning` runs at the end of `setup()`.
- **`wifi ...` uses `|`, not a space,** to delimit SSID and password. `wifi Home Net|pw`
  parses SSID `Home Net`, password `pw`.

## 9. Verification
- **On-device, first boot:** flashed a board with no saved WiFi; confirmed serial prints
  `PROV: BLE provisioning started ...` and the device is discoverable over BLE as
  `PROV_HomeControl`. Completed provisioning from the app and confirmed
  `PROV: WiFi credentials accepted` → `WiFi: connected ip=...` → `PROV: provisioning
  finished (BLE released)`, followed by free-heap large enough for the MQTT/TLS connect.
- **`prov`:** typed `prov` in the serial monitor; device rebooted and came back advertising
  `PROV_HomeControl`.
- **`reset`:** typed `reset`; confirmed broker config and WiFi were gone after reboot and
  the device went straight to provisioning.
- **Auto-fallback:** provisioned to a network, then powered the AP off before the next
  boot; confirmed `WiFi: no connection in 90s — rebooting into BLE provisioning` at ~90 s
  and the device re-entered provisioning.
- **Serial recovery:** set WiFi with `wifi <SSID>|<PASSWORD>` (SSID containing a space) and
  confirmed it connected and persisted across a reboot.

## 10. Related
- App counterpart: `HomeControl/docs/features/ble-provisioning-app.md` (BLE scan, PoP
  entry, credential push).
- `wifi-broker-config.md` — the NVS-persisted broker config and the serial `set`/`wifi`
  commands (same `handleSerialConfig()` function).
- `mqtt-control.md` — what happens after WiFi connects (topics, connect, Last-Will).
- `telemetry.md`, `device-commands.md` — post-connect device features.
- `partitions.md` — why the `nvs` partition (and thus provisioned WiFi) survives OTA.
