# WiFi & Broker Config (NVS + Serial)

> **Status:** shipped · **Repo(s):** firmware · **Since:** firmware v2.6.0 (`src/main.cpp`)

Recreation-grade spec: someone (or Claude) with only this document should be able to
rebuild this feature identically. Prefer concrete detail over prose. Keep it current —
update this file in the same commit that changes the feature.

## 1. Summary
The device carries **no credentials in its firmware image**. The MQTT broker host, port,
username, password, and the device id all live in **NVS flash** (Espressif's `Preferences`
key/value store, namespace `"homecfg"`), and WiFi credentials live in the core's own WiFi
NVS. This doc covers the runtime-config layer: the five config globals, `loadConfig()` /
`saveConfig()`, and the USB-serial command handler `handleSerialConfig()` that provisions
or replaces the broker (`set …`), sets WiFi as a recovery path (`wifi …`), and forces a
re-provision or full wipe (`prov` / `reset`). The design rule is: **config is runtime, never
compiled in** — the same binary flashes to every board, and each board is personalized
after the fact over serial (or WiFi over BLE, see `ble-provisioning.md`).

## 2. User-facing behavior
Over a 115200-baud USB serial monitor (PlatformIO `monitor_speed = 115200`), a technician
types one command per line and presses Enter:

- **`set host=<h> port=8883 user=<u> pass=<p> id=dev1`** — sets any subset of broker
  fields (order/space-separated `key=value` pairs), saves to NVS, and — if a host is now
  present — reconnects MQTT immediately with the new settings. Prints
  `CFG:  saved from serial  host=… port=… user=… devid=…`.
- **`wifi <SSID>|<PASSWORD>`** — recovery/dev path to join WiFi without the BLE app. The
  `|` separator lets SSIDs/passwords contain spaces. Saves creds to WiFi NVS and connects.
- **`prov`** — reboot into BLE WiFi provisioning (see `ble-provisioning.md`).
- **`reset`** — wipe **all** `homecfg` NVS keys and stored WiFi creds, then reboot into
  provisioning. A factory reset.

At boot the device prints its current config (`CFG:  host=… port=… user=… devid=…`, host
shown as `(unset)` when empty). If no broker is configured, MQTT logs a one-line hint
telling the operator exactly what `set …` command to type. Serial commands are accepted
continuously from boot onward — there is no button and no fixed timeout to race.

## 3. Architecture & files
| File | Role |
|------|------|
| `src/main.cpp` | Config globals, `loadConfig()`, `saveConfig()`, `setForceProv()`/`takeForceProv()`, `handleSerialConfig()`, and the `loop()`/`setup()` wiring that drives them. |
| `platformio.ini` | `monitor_speed = 115200` (matches `Serial.begin(115200)`); partitions preserve NVS across OTA (see `partitions.md`). |
| NVS partition | Physical store. Namespace `"homecfg"` holds broker config + the `forceprov` flag; the WiFi creds live in the core's separate WiFi NVS namespace. |

How it plugs in: `loadConfig()` runs once in `setup()` after the GPIO init and before TLS/
MQTT setup, so `MQTT_HOST`/`MQTT_PORT` are known before `mqtt.setServer()`. `handleSerialConfig()`
runs at the top of **every** `loop()` iteration; when it returns `true` (a new host was set)
`loop()` re-arms the server and drops any live MQTT connection so the next `connectMQTT()`
uses the new broker.

## 4. Data model / types
The five runtime-config globals, with their compiled-in **defaults** (used only until NVS
overrides them) and buffer sizes:

```cpp
char     DEVICE_ID[32] = "dev1";   // unique per board  -> "<id>" in every MQTT topic
char     MQTT_HOST[80] = "";       // e.g. abcd1234.s1.eu.hivemq.cloud
uint16_t MQTT_PORT     = 8883;     // HiveMQ Cloud TLS port
char     MQTT_USER[48] = "";
char     MQTT_PASS[64] = "";
```

**NVS layout** — `Preferences` namespace `"homecfg"`:

| NVS key | Type | Global | Default (if key absent) |
|---------|------|--------|-------------------------|
| `host` | String | `MQTT_HOST` | current global (`""`) |
| `user` | String | `MQTT_USER` | current global (`""`) |
| `pass` | String | `MQTT_PASS` | current global (`""`) |
| `devid` | String | `DEVICE_ID` | current global (`"dev1"`) |
| `port` | UShort | `MQTT_PORT` | current global (`8883`) |
| `forceprov` | Bool | — | `false` (one-shot "reboot into BLE provisioning" flag) |

WiFi SSID/password are **not** in `homecfg`; they are stored by the Arduino-ESP32 WiFi
stack in its own NVS (written via BLE provisioning or `WiFi.persistent(true)` in the `wifi`
command). `reset` clears both.

## 5. Protocol / contract
This layer has no MQTT topics of its own. Its output is the `DEVICE_ID` that becomes the
`<id>` in every `home/<id>/…` topic (see `mqtt-control.md`) and the host/port/credentials
that `PubSubClient` connects with. The USB-serial "contract" — the exact command grammar —
is:

```
set  host=<h> port=<n> user=<u> pass=<p> id=<devid>   # any subset, space-separated
wifi <SSID>|<PASSWORD>                                 # '|' splits ssid/pass
prov                                                   # reboot into BLE provisioning
reset                                                  # wipe all config + wifi, reboot to prov
```

Keys recognized by `set`: `host`, `port`, `user`, `pass`, `id`. Anything else in a `set`
line is ignored. Lines are read until `\n` and `trim()`-med.

## 6. Key logic

### Load (`loadConfig()`) — read-only open
```cpp
void loadConfig() {
  prefs.begin("homecfg", true);   // read-only
  strlcpy(MQTT_HOST, prefs.getString("host", MQTT_HOST).c_str(), sizeof(MQTT_HOST));
  strlcpy(MQTT_USER, prefs.getString("user", MQTT_USER).c_str(), sizeof(MQTT_USER));
  strlcpy(MQTT_PASS, prefs.getString("pass", MQTT_PASS).c_str(), sizeof(MQTT_PASS));
  strlcpy(DEVICE_ID, prefs.getString("devid", DEVICE_ID).c_str(), sizeof(DEVICE_ID));
  MQTT_PORT = prefs.getUShort("port", MQTT_PORT);
  prefs.end();
}
```
Opened **read-only** (`begin(..., true)`) so a load can never create or dirty the namespace.
Each `getString`/`getUShort` passes the **current global as its own default**, so an absent
key leaves the compiled-in default (`""`, `8883`, `dev1`) intact. `strlcpy` with the buffer
size guarantees NUL-termination and no overflow even if a stored value is longer than the
fixed buffer.

### Save (`saveConfig()`) — read-write open
```cpp
void saveConfig() {
  prefs.begin("homecfg", false);  // read-write
  prefs.putString("host", MQTT_HOST);
  prefs.putString("user", MQTT_USER);
  prefs.putString("pass", MQTT_PASS);
  prefs.putString("devid", DEVICE_ID);
  prefs.putUShort("port", MQTT_PORT);
  prefs.end();
}
```
Writes all five keys from the globals. Always paired with a matching `end()` so the NVS
handle is released (NVS handles are a limited resource).

### Force-provision flag (survives the reboot)
```cpp
void setForceProv() { prefs.begin("homecfg", false); prefs.putBool("forceprov", true); prefs.end(); }
bool takeForceProv() {
  prefs.begin("homecfg", false);
  bool f = prefs.getBool("forceprov", false);
  if (f) prefs.putBool("forceprov", false);   // clear so it only fires once
  prefs.end();
}
```
A one-shot cross-reboot signal: `prov`/`reset`/the 90 s WiFi-timeout all `setForceProv()`
then restart; `setup()` calls `takeForceProv()` which reads-and-clears it so the next boot
re-provisions exactly once and then behaves normally. *Rationale:* provisioning can't start
before a reboot cleanly frees the WiFi/BT state, so intent is persisted and consumed on the
next boot.

### Serial command handler (`handleSerialConfig()`)
```cpp
bool handleSerialConfig() {
  if (!Serial.available()) return false;
  String line = Serial.readStringUntil('\n');
  line.trim();
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
  if (line.startsWith("wifi ")) {                 // recovery: set WiFi without the app
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
  if (!line.startsWith("set ")) return false;

  for (int i = 4; i < (int)line.length(); ) {     // parse key=value pairs after "set "
    int sp = line.indexOf(' ', i);
    if (sp < 0) sp = line.length();
    String kv = line.substring(i, sp);
    int eq = kv.indexOf('=');
    if (eq > 0) {
      String k = kv.substring(0, eq), v = kv.substring(eq + 1);
      if      (k == "host") strlcpy(MQTT_HOST, v.c_str(), sizeof(MQTT_HOST));
      else if (k == "user") strlcpy(MQTT_USER, v.c_str(), sizeof(MQTT_USER));
      else if (k == "pass") strlcpy(MQTT_PASS, v.c_str(), sizeof(MQTT_PASS));
      else if (k == "id")   strlcpy(DEVICE_ID, v.c_str(), sizeof(DEVICE_ID));
      else if (k == "port") MQTT_PORT = (uint16_t) v.toInt();
    }
    i = sp + 1;
  }
  saveConfig();
  logf("CFG:  saved from serial  host=%s port=%u user=%s devid=%s",
       MQTT_HOST, MQTT_PORT, MQTT_USER, DEVICE_ID);
  return MQTT_HOST[0] != 0;                        // true -> caller reconnects MQTT
}
```
Dispatch order and reasoning:
1. **Guard** — returns immediately if no serial byte is waiting, so it's cheap to call
   every loop.
2. **`prov` / `reset`** — exact matches, checked first because they reboot. `reset` does a
   full `prefs.clear()` **and** `WiFi.disconnect(true, true)` (the two `true`s erase the
   stored WiFi creds and turn the radio off) before setting the force-prov flag.
3. **`wifi …`** — prefix match; splits on the first `|` so SSID/password may contain
   spaces (a space separator would break real-world SSIDs). `WiFi.persistent(true)` makes
   the core save the creds to WiFi NVS. Returns `false` (broker unchanged).
4. **`set …`** — hand-rolled `key=value` tokenizer (space-delimited). Only the whitelisted
   keys are applied; `port` goes through `toInt()`→`uint16_t`. Then `saveConfig()` persists
   everything.
5. **Return value** = `MQTT_HOST[0] != 0`: `true` signals `loop()` that a usable broker is
   configured so it should re-point and reconnect MQTT.

`loop()` acts on the return:
```cpp
if (handleSerialConfig()) {            // broker changed over serial -> reconnect
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  if (mqtt.connected()) mqtt.disconnect();
}
```

### Boot-time serial provisioning window
There is no timed lockout window — serial provisioning is *always open*:
- `setup()`: `Serial.begin(115200); Serial.setTimeout(200); delay(300);` — the 200 ms
  `setTimeout` bounds `readStringUntil('\n')` so a partial line can't hang the loop, and
  the 300 ms delay lets the USB CDC port enumerate before the boot banner prints.
- `setup()` then calls `loadConfig()` and logs the current config, so the operator
  immediately sees whether a broker is set.
- From the first `loop()` onward, `handleSerialConfig()` is polled every iteration, so the
  operator can type `set …` / `wifi …` / `prov` / `reset` at any time — at first boot
  (before any broker is known) or years later to re-point the device. When no broker is set,
  `connectMQTT()` prints the exact `set host=.. port=8883 user=.. pass=..` hint and
  `delay(3000)`, effectively a repeating prompt while you provision.

## 7. Dependencies & setup
- **`Preferences.h`** (`Preferences prefs;`) — Arduino-ESP32 core wrapper over NVS. No
  `lib_deps` entry needed.
- **`WiFi.h`** — for the `wifi`/`reset` paths (`WiFi.begin/persistent/disconnect/mode`).
- **`monitor_speed = 115200`** in `platformio.ini` must match `Serial.begin(115200)`.
- **Partition table** (`partitions.md`) keeps the `nvs` region at the same offset as the
  stock `huge_app` table, so an OTA update **preserves** this saved config — the device
  keeps its broker/id/WiFi across firmware upgrades.
- **Rationale (why runtime, not compiled-in):** (a) one firmware binary flashes to every
  board — no per-device rebuild; (b) no secrets (broker password, WiFi password) ever land
  in source control or the shipped image; (c) the broker can be re-pointed in the field
  without a reflash; (d) OTA-safe because NVS survives the update.

## 8. Edge cases & gotchas
- **Read-only vs read-write open:** `loadConfig()` uses `begin("homecfg", true)`. If you
  ever `putX()` on a read-only handle it silently fails — writes must use the `false` form.
- **Default-is-current-global trick:** `getString("host", MQTT_HOST)` means an absent key
  yields the *existing* global, not an empty string. Keep the global defaults sane because
  they are the true fallback.
- **`set` with unknown keys / bad `port`:** unknown keys are ignored; a non-numeric `port`
  becomes `0` via `toInt()` (would then fail to connect — set it explicitly to `8883`).
- **`wifi` needs the `|`:** without a `|` the whole remainder is treated as the SSID and
  the password is empty (fine for open networks, wrong for WPA).
- **`reset` is destructive:** it wipes the broker config *and* WiFi creds and forces
  provisioning; the device is unreachable over MQTT until re-provisioned.
- **NVS handle leaks:** every `begin()` is matched with `end()`; forgetting `end()` exhausts
  NVS handles. All paths here pair them.
- **DEVICE_ID uniqueness:** two boards sharing `devid` collide on the MQTT client id (see
  `mqtt-control.md` §8). Always `set id=…` per board.
- **Serial line endings:** the handler splits on `\n` and `trim()`s, so `\r\n` from most
  terminals is tolerated; a terminal sending no newline never dispatches.

## 9. Verification
- **Fresh board:** boot log shows `CFG:  host=(unset) port=8883 user= devid=dev1` and MQTT
  prints the `no broker configured` hint. Type `set host=… port=8883 user=… pass=… id=dev1`
  → log shows `CFG:  saved from serial …` and MQTT connects within a few seconds.
- **Persistence:** power-cycle after `set …`; the boot `CFG:` line shows the saved values
  (proving NVS round-trip), and MQTT reconnects without re-entering anything.
- **WiFi recovery:** with the BLE app unavailable, `wifi MyNet|s3cr3t pw` → serial logs
  `CMD: setting WiFi over serial …` then `WiFi: connected ip=…`.
- **Force-prov one-shot:** `prov` reboots into BLE provisioning once; the following normal
  reboot does **not** re-provision (confirms `takeForceProv()` clears the flag).
- **Factory reset:** `reset` → boot shows defaults again and the device advertises
  `PROV_HomeControl` over BLE (see `ble-provisioning.md`).
- **OTA-safe config:** after an OTA update, the `CFG:` line still shows the pre-update
  broker/id (confirms the `nvs` partition offset is preserved).

## 10. Related
- [`ble-provisioning.md`](ble-provisioning.md) — the primary WiFi setup path (`prov` reboots into it); the `wifi …` serial command is its fallback.
- [`mqtt-control.md`](mqtt-control.md) — consumes `MQTT_HOST/PORT/USER/PASS` and `DEVICE_ID`.
- [`partitions.md`](partitions.md) — why the `nvs` partition (and this config) survives OTA.
- App: [`broker-config.md`](../../../../../HomeControl/docs/features/broker-config.md) — the app-side HiveMQ connection + gitignored credentials.
- App: [`ble-provisioning-app.md`](../../../../../HomeControl/docs/features/ble-provisioning-app.md) — the in-app BLE WiFi setup this pairs with.
