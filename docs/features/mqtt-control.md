# MQTT Control Layer (TLS)

> **Status:** shipped · **Repo(s):** firmware · **Since:** firmware v2.6.0 (`src/main.cpp`)

Recreation-grade spec: someone (or Claude) with only this document should be able to
rebuild this feature identically. Prefer concrete detail over prose. Keep it current —
update this file in the same commit that changes the feature.

## 1. Summary
This is the device's remote-control nervous system: a `PubSubClient` MQTT client running
over a TLS socket (`WiFiClientSecure`) to **HiveMQ Cloud on port 8883**. It connects with
a **Last-Will** so the broker announces the device `offline` if it drops, publishes
`online` on connect, subscribes to a small fixed set of command topics
(`home/<id>/+/set`, `restart`, `identify`, `ota`), and mirrors every channel's on/off as a
**retained** `state` topic so a freshly-opened app immediately sees the true state. All
control is one-way-simple text payloads (`on`/`off`/`toggle`); there is no local button
UI. The broker host/credentials are not compiled in — they come from NVS/serial (see
`wifi-broker-config.md`).

## 2. User-facing behavior
The user acts entirely through the app (`mqtt-state-layer.md` app-side):

- Toggling a device in the app publishes `on`/`off`/`toggle` to `home/<id>/<channel>/set`;
  the physical output changes within a network round-trip and the app tile confirms from
  the retained `state` echo.
- When the app (or any MQTT client) connects, it sees each channel's **last known**
  on/off immediately because `state` topics are retained, and it sees whether the device
  is reachable from the retained `home/<id>/status` (`online`/`offline`).
- If the device loses power or WiFi, the broker publishes the Last-Will `offline` after
  the keep-alive lapses, and the app shows the device as offline without the device doing
  anything.
- Command topics let the app restart the device, make it blink to identify itself, or
  push an OTA firmware URL.

## 3. Architecture & files
| File | Role |
|------|------|
| `src/main.cpp` | Everything: the `WiFiClientSecure net` + `PubSubClient mqtt(net)` globals, `setup()` TLS/buffer/callback wiring, `connectMQTT()`, `onMessage()`, `publishState()`, `mqttStateStr()`, and `loop()` service. |
| `platformio.ini` | `lib_deps = knolleary/PubSubClient@^2.8`. `WiFiClientSecure` ships with the Arduino-ESP32 core. |
| `wifi-broker-config.md` | Supplies `MQTT_HOST`/`MQTT_PORT`/`MQTT_USER`/`MQTT_PASS`/`DEVICE_ID` (NVS + serial). |
| `channels-gpio.md` | The `channels[]` the `set`/`state` topics act on. |

Globals and construction:
```cpp
WiFiClientSecure net;          // TLS socket
PubSubClient mqtt(net);        // MQTT speaks over the TLS socket
```
`PubSubClient` is transport-agnostic: it takes a `Client&`. By handing it a
`WiFiClientSecure`, every MQTT byte is TLS-encrypted with no MQTT-layer changes.

## 4. Data model / types
No persisted model of its own; it reads the runtime config globals (see
`wifi-broker-config.md`) and the shared `channels[]` (see `channels-gpio.md`):

```cpp
char     DEVICE_ID[32] = "dev1";   // -> the "<id>" in every topic
char     MQTT_HOST[80] = "";       // e.g. abcd1234.s1.eu.hivemq.cloud
uint16_t MQTT_PORT     = 8883;     // HiveMQ Cloud TLS port
char     MQTT_USER[48] = "";
char     MQTT_PASS[64] = "";
```

Topic strings are built ad-hoc with `snprintf`/`String` into fixed 80-byte buffers, e.g.:
```cpp
char t[80];
snprintf(t, sizeof(t), "home/%s/%s/state", DEVICE_ID, c.name);
```

## 5. Protocol / contract
Base namespace is `home/<id>/…` where `<id>` is `DEVICE_ID`. **PubSubClient is QoS-0 only**
for both publish and subscribe (it has no QoS 1/2 delivery), so every row below is **QoS 0**.

| Topic | Direction | Payload | Retained | QoS | Purpose |
|-------|-----------|---------|:--------:|:---:|---------|
| `home/<id>/status` | device → app | `online` / `offline` | **yes** | 0 | Presence. `offline` is the **Last-Will** (also published on connect=`online`, and before an MQTT-triggered restart). |
| `home/<id>/<ch>/set` | app → device | `on` / `off` / `toggle` | no | 0 | Drive a channel. Subscribed via wildcard `home/<id>/+/set`. `<ch>` ∈ {red, green, blue, relay1..relay4}. |
| `home/<id>/<ch>/state` | device → app | `on` / `off` | **yes** | 0 | Authoritative channel state. Published on every change and re-published for all channels on connect. |
| `home/<id>/restart` | app → device | (any) | — | 0 | Publish `status=offline` then `ESP.restart()`. |
| `home/<id>/identify` | app → device | (any) | — | 0 | Blink RGB to locate the board (`identifyBlink()`). |
| `home/<id>/ota` | app → device | firmware image URL | — | 0 | Trigger HTTPS OTA (`doOTA()`), see `ota.md`. |
| `home/<id>/ota/status` | device → app | progress/result text | **yes** | 0 | OTA feedback. See `ota.md`. |
| `home/<id>/telemetry` | device → app | JSON `{fw,rssi,uptime,heap}` | **yes** | 0 | Health. See `telemetry.md`. |

**Last-Will (LWT):** registered in the `connect()` call — topic `home/<id>/status`, QoS 0,
retain `true`, payload `offline`. If the TCP/TLS session dies without a clean DISCONNECT,
the broker publishes it after the keep-alive interval expires.

The app-side counterpart of this contract is
[`mqtt-state-layer.md`](../../../../../HomeControl/docs/features/mqtt-state-layer.md).

## 6. Key logic

### One-time setup (`setup()`)
```cpp
net.setInsecure();          // skip TLS cert validation (fine for a home broker)
mqtt.setBufferSize(2048);   // roomy enough for long OTA image URLs
mqtt.setCallback(onMessage);
if (MQTT_HOST[0]) mqtt.setServer(MQTT_HOST, MQTT_PORT);
```
- **`net.setInsecure()`** — TLS is used for confidentiality/encryption, but the server
  certificate is **not** validated against a CA. *Rationale:* no CA bundle is compiled in,
  the broker host is user-configured at runtime, and this is a home LAN/cloud broker. It
  removes the need to ship/rotate root certs and to keep the device clock accurate for
  cert validity. (Trade-off: no protection against an active MITM; acceptable here. The
  same `setInsecure()` is used for the OTA HTTPS client for consistency.)
- **`mqtt.setBufferSize(2048)`** — PubSubClient's default `MQTT_MAX_PACKET_SIZE` is 256
  bytes, which caps **both** the largest publish and the largest *incoming* message. OTA
  URLs (e.g. GitHub release-asset redirects) and the telemetry JSON can exceed 256, and an
  over-size incoming publish is silently dropped by PubSubClient. 2048 is a safe headroom
  that still fits comfortably in RAM. *This is the "why 2048" the template asks for.*
- **`setServer` is guarded** by `MQTT_HOST[0]` so a device with no broker yet configured
  doesn't point the client at an empty host; the serial handler re-arms it later.

### Connect (`connectMQTT()`)
```cpp
void connectMQTT() {
  if (!MQTT_HOST[0]) {
    logf("MQTT: no broker configured — send over serial: set host=.. port=8883 user=.. pass=..");
    delay(3000);
    return;
  }
  char statusTopic[80];
  snprintf(statusTopic, sizeof(statusTopic), "home/%s/status", DEVICE_ID);

  while (!mqtt.connected()) {
    logf("MQTT: connecting to %s:%u (TLS) as \"%s\" ...", MQTT_HOST, MQTT_PORT, DEVICE_ID);
    bool ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS,
                           statusTopic, 0, true, "offline");   // LWT: retain, "offline"
    if (ok) {
      logf("MQTT: connected");
      mqtt.publish(statusTopic, "online", true);               // retained presence
      char sub[80];
      snprintf(sub, sizeof(sub), "home/%s/+/set", DEVICE_ID);
      mqtt.subscribe(sub);                                     // all channel sets
      logf("MQTT: subscribed to %s", sub);
      char restartTopic[80];
      snprintf(restartTopic, sizeof(restartTopic), "home/%s/restart", DEVICE_ID);
      mqtt.subscribe(restartTopic);
      char identifyTopic[80];
      snprintf(identifyTopic, sizeof(identifyTopic), "home/%s/identify", DEVICE_ID);
      mqtt.subscribe(identifyTopic);
      char otaTopic[80];
      snprintf(otaTopic, sizeof(otaTopic), "home/%s/ota", DEVICE_ID);
      mqtt.subscribe(otaTopic);
      for (size_t i = 0; i < NUM_CH; i++) publishState(channels[i]);  // re-sync app
      publishTelemetry();
    } else {
      logf("MQTT: connect failed rc=%d (%s), retry in 3s",
           mqtt.state(), mqttStateStr(mqtt.state()));
      delay(3000);
      return;
    }
  }
}
```
Connect-order rationale:
1. **`connect(clientId, user, pass, willTopic, willQos=0, willRetain=true, willMessage="offline")`** —
   the client id is `DEVICE_ID` (must be unique per board or the broker will kick the
   older session). The Last-Will is registered *in* the connect packet — this is the only
   place a broker will accept a will, so it must be set before anything else.
2. **Publish `online` (retained)** immediately, so a client connecting slightly later
   still reads presence from the retained value.
3. **Subscribe** to the wildcard `home/<id>/+/set` (covers all seven channels with one
   subscription, and auto-covers channels added later) plus the three exact command
   topics. The exact topics are *not* matched by `+/set`, so they need their own
   subscribes.
4. **Re-publish all channel `state`s (retained)** — this is what makes the app show the
   real state after a device reboot, since channel state is not persisted on the device
   (see `channels-gpio.md` §8).
5. **`publishTelemetry()`** once so the detail sheet has data immediately.

On failure it logs a **decoded** reason and returns after `delay(3000)` (it does not spin
forever here — `loop()` calls `connectMQTT()` again next iteration). The `while` loop
exists so a transient mid-handshake state still resolves, but every real failure path
`return`s to keep `loop()` responsive to serial commands and WiFi events.

### Dispatch (`onMessage()`)
```cpp
void onMessage(char *topic, byte *payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  logf("MQTT: rx  %s = \"%s\"", topic, msg.c_str());

  String t = topic;
  if (t == String("home/") + DEVICE_ID + "/ota")      { doOTA(msg); return; }
  if (t == String("home/") + DEVICE_ID + "/identify") { identifyBlink(); return; }
  if (t == String("home/") + DEVICE_ID + "/restart") {
    char statusTopic[80];
    snprintf(statusTopic, sizeof(statusTopic), "home/%s/status", DEVICE_ID);
    mqtt.publish(statusTopic, "offline", true);   // tell the app before we go
    delay(300);
    ESP.restart();
  }
  for (size_t i = 0; i < NUM_CH; i++) {
    String setTopic = String("home/") + DEVICE_ID + "/" + channels[i].name + "/set";
    if (t == setTopic) {
      if      (msg == "on")     applyChannel(channels[i], true);
      else if (msg == "off")    applyChannel(channels[i], false);
      else if (msg == "toggle") applyChannel(channels[i], !channels[i].state);
      else    logf("      ignored: \"%s\" is not on/off/toggle", msg.c_str());
      return;
    }
  }
  logf("      ignored: no channel matches this topic");
}
```
Dispatch order is **command topics first, channels last**. The payload is copied to a
`String` and `trim()`-med so trailing `\r\n` from a hand-typed publish doesn't break the
`==` comparisons. Command topics use exact-string matching; channels loop over
`channels[]` matching `home/<id>/<name>/set`. `toggle` reads current `state`. Unknown
payloads/topics are logged and ignored (no crash, no partial action). **Restart publishes
a retained `offline` before rebooting** so the app flips to offline instantly instead of
waiting for the keep-alive-driven Last-Will.

### Publish state (`publishState()`)
```cpp
void publishState(Channel &c) {
  char t[80];
  snprintf(t, sizeof(t), "home/%s/%s/state", DEVICE_ID, c.name);
  mqtt.publish(t, c.state ? "on" : "off", true);   // retained
  logf("MQTT: tx  %s = %s", t, c.state ? "on" : "off");
}
```
Always **retained**, payload is the literal `on`/`off`. Called by `applyChannel()` after
every change, by the connect re-sync loop, and by `identifyBlink()`'s restore. Retention
is the whole reason the app can be stateless about history — the broker holds the last
value.

### Error decoding (`mqttStateStr()`)
Turns `PubSubClient::state()` return codes into human-readable log text so a failed
connect explains itself on the serial console:
```cpp
const char *mqttStateStr(int s) {
  switch (s) {
    case -4: return "timeout";
    case -3: return "connection lost";
    case -2: return "connect failed (bad host/port, TLS, or wrong network?)";
    case -1: return "disconnected";
    case  0: return "connected";
    case  1: return "bad protocol";
    case  2: return "bad client id";
    case  3: return "server unavailable";
    case  4: return "bad credentials (check MQTT username/password)";
    case  5: return "not authorized";
    default: return "unknown";
  }
}
```
Mapping (PubSubClient constants): `-4 MQTT_CONNECTION_TIMEOUT`, `-3 MQTT_CONNECTION_LOST`,
`-2 MQTT_CONNECT_FAILED` (TCP/TLS couldn't open — most often wrong host/port or TLS),
`-1 MQTT_DISCONNECTED`, `0 MQTT_CONNECTED`, `1 MQTT_CONNECT_BAD_PROTOCOL`,
`2 MQTT_CONNECT_BAD_CLIENT_ID`, `3 MQTT_CONNECT_UNAVAILABLE`,
`4 MQTT_CONNECT_BAD_CREDENTIALS`, `5 MQTT_CONNECT_UNAUTHORIZED`. The `-2` vs `4` split is
the practical one: `-2` = network/TLS problem, `4` = HiveMQ username/password wrong.

### Service loop
`loop()` only touches MQTT once WiFi is up:
```cpp
if (!mqtt.connected()) connectMQTT();
mqtt.loop();                 // pump keep-alives + deliver incoming to onMessage()
```
`mqtt.loop()` must be called frequently or the broker drops the session; every ~30 s the
loop also republishes telemetry. If the broker changes over serial, `loop()`'s serial
handler calls `mqtt.setServer(...)` and disconnects so the next `connectMQTT()` uses the
new host (see `wifi-broker-config.md`).

## 7. Dependencies & setup
- **`knolleary/PubSubClient@^2.8`** — the MQTT client (`platformio.ini` `lib_deps`).
- **`WiFiClientSecure`** — TLS transport, part of the Arduino-ESP32 core (no lib_deps line).
- **`mqtt.setBufferSize(2048)`** is mandatory config, not optional tuning (see §6).
- Broker credentials/host come from NVS/serial (`wifi-broker-config.md`), never compiled in.
- HiveMQ Cloud requires TLS on **8883**; plaintext 1883 is not offered, which is why the
  transport is `WiFiClientSecure` and not `WiFiClient`.

## 8. Edge cases & gotchas
- **QoS 0 only** — no delivery guarantee, no dup handling. Fine because `state` is
  retained (a missed message self-heals on the next publish/connect) and `set` is
  user-driven (the user just taps again). Do not assume a `set` is guaranteed delivered.
- **256-byte default buffer would silently drop** long OTA URLs / telemetry *inbound* and
  truncate *outbound*. If OTA "does nothing," check `setBufferSize` first.
- **Duplicate `DEVICE_ID`** — two boards with the same id fight over one broker session,
  causing connect/disconnect flapping. Ids must be unique (set via `set id=..`).
- **`setInsecure()`** — encrypted but unauthenticated server. Not safe against an active
  MITM; a deliberate trade-off for a keyless home device.
- **Restart-before-Will race** — `restart` publishes `offline` then `delay(300)` before
  `ESP.restart()` to give the packet time to leave; if the socket is already dead the app
  still gets the Last-Will eventually.
- **Empty host** — `connectMQTT()` returns early with a serial hint instead of connecting
  to `":8883"`; `setServer` is likewise skipped until a host exists.
- **`onMessage` runs inside `mqtt.loop()`** — blocking actions (`identifyBlink()` ~1 s,
  `doOTA()` much longer) stall MQTT keep-alives while they run; acceptable because they're
  terminal (identify is brief; OTA reboots).

## 9. Verification
- **Live broker (HiveMQ Cloud):** with `mosquitto_sub`/HiveMQ web client subscribed to
  `home/<id>/#`, connecting the device shows retained `status=online`, seven retained
  `.../state` values, and one `telemetry` JSON.
- **Presence/LWT:** pull power → after the keep-alive the broker emits retained
  `status=offline` with no device action. Send `home/<id>/restart` → device publishes
  `offline` immediately, then reconnects `online`.
- **Control:** publish `on`/`off`/`toggle` to `home/<id>/red/set` → serial shows
  `MQTT: rx …` then `CH: …` then `MQTT: tx home/<id>/red/state = on`.
- **Error decode:** wrong password → serial logs `connect failed rc=4 (bad credentials …)`;
  wrong host → `rc=-2 (connect failed …)`.
- **Buffer:** publish a >256-char URL to `home/<id>/ota` and confirm `doOTA` receives the
  full string (`OTA: updating from …`).

## 10. Related
- [`channels-gpio.md`](channels-gpio.md) — the `set`/`state` channel model this drives.
- [`wifi-broker-config.md`](wifi-broker-config.md) — where `MQTT_HOST/PORT/USER/PASS/DEVICE_ID` come from.
- [`ble-provisioning.md`](ble-provisioning.md) — how WiFi (prerequisite for MQTT) is set up.
- [`device-commands.md`](device-commands.md) — `restart` + `identify` command semantics.
- [`telemetry.md`](telemetry.md) — the `telemetry` topic payload.
- [`ota.md`](ota.md) — the `ota` / `ota/status` topics and the update flow.
- App: [`mqtt-state-layer.md`](../../../../../HomeControl/docs/features/mqtt-state-layer.md) — the app's state engine (the other half of this contract).
- App: [`broker-config.md`](../../../../../HomeControl/docs/features/broker-config.md) — the app's HiveMQ connection + credentials.
