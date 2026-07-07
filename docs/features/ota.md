# HTTPS OTA (firmware update over the air)

> **Status:** shipped · **Repo(s):** firmware (pairs with app `ota-app.md`) · **Since:** `FW_VERSION 2.6.0` (`src/main.cpp`)

Recreation-grade spec: someone (or Claude) with only this document should be able to
rebuild this feature identically. This is the **firmware half**. The app half — how the
phone builds the image URL, publishes it, and renders progress — is documented in
`HomeControl/docs/features/ota-app.md`.

## 1. Summary

The device can flash a new firmware image over the air, with no USB cable. The app
publishes a firmware image URL to `home/<id>/ota`; the device downloads it over HTTPS
with `httpUpdate` (Arduino-ESP32's `HTTPUpdate`), writes it into the inactive OTA app
slot, flips the boot pointer, and reboots into the new firmware. Throughout, it publishes
human-readable progress to `home/<id>/ota/status` (`starting` → `downloading 25/50/75%`
→ `error …` on failure, or a silent reboot into the new image on success). Images are
served from **public GitHub Releases**, which give stable, unauthenticated download URLs.
This depends critically on a two-slot OTA partition table (see `partitions.md`) and on
the MQTT receive buffer being large enough to carry a long image URL (see §6/§8).

## 2. User-facing behavior

From the device's point of view (the app-facing UX lives in `ota-app.md`):

1. The device is running and MQTT-connected. On connect it subscribes to `home/<id>/ota`
   (and republishes telemetry, so the app knows the current `fw` version).
2. A retained message arrives on `home/<id>/ota` whose payload is a firmware image URL.
3. The device publishes `starting` to `home/<id>/ota/status`, then downloads the image,
   publishing `downloading 0%`, `downloading 25%`, `downloading 50%`, `downloading 75%`
   (roughly every 25%) as it goes.
4. On success the new image is written and the device **reboots into it before returning**
   — the app observes the Last-Will `offline` on `home/<id>/status`, then `online` again a
   few seconds later, and a bumped `fw` in the next telemetry payload.
5. On failure it publishes `error <n>: <reason>` (or `error: no url` / `no update`) to
   `home/<id>/ota/status` and keeps running the current firmware — a failed download never
   bricks the device, because the running slot is untouched until the new image is fully
   verified.

Everything is visible in the serial log too (tag `OTA:`), e.g.:

```
[   61.204] OTA:  updating from https://github.com/<owner>/<repo>/releases/download/fw-2.6.0/firmware.bin
[   61.205] OTA:  starting
[   62.9xx] OTA:  downloading 0%
[   6x.xxx] OTA:  downloading 25%
...
(reboots into the new image)
```

## 3. Architecture & files

| File | Role |
|------|------|
| `src/main.cpp` | The entire feature. OTA include, `FW_VERSION`, the `doOTA()` forward declaration, `publishOtaStatus()`, `doOTA()`, the `home/<id>/ota` subscription in `connectMQTT()`, the dispatch in `onMessage()`, and the `mqtt.setBufferSize(2048)` that makes long URLs deliverable. |
| `partitions.csv` | The two-slot OTA partition table (`ota_0` / `ota_1` + `otadata`) that OTA physically requires. Documented in `partitions.md`. |
| `platformio.ini` | `board_build.partitions = partitions.csv` wires that table into the build. |
| GitHub Releases (external) | Hosts `firmware.bin` at a stable, public HTTPS URL per release tag `fw-<version>`. |

How it plugs in: OTA is just another MQTT command topic. It reuses the same TLS/MQTT
machinery as channel control (`WiFiClientSecure` + `PubSubClient`), the same
`home/<id>/…` topic scheme, and the same retained-status pattern. The only new transport
is the HTTPS download, which uses a **separate** `WiFiClientSecure` created inside
`doOTA()` (not the MQTT socket).

Relevant `#include`s (top of `main.cpp`):

```cpp
#include <HTTPClient.h>
#include <HTTPUpdate.h>   // over-the-air firmware update over HTTPS
```

`HTTPUpdate.h` provides the global `httpUpdate` instance used below.

## 4. Data model / types

There is no persistent data model — OTA is stateless from flash's perspective (the OTA
bookkeeping lives in the `otadata` partition, managed by the bootloader; see
`partitions.md`). The in-firmware surface is:

```cpp
#define FW_VERSION "2.6.0"   // bumped every release; reported in telemetry
```

`FW_VERSION` is the string the app compares against the latest release to decide whether
an update is available (see `ota-app.md`). It is published in the telemetry JSON:

```cpp
snprintf(payload, sizeof(payload),
         "{\"fw\":\"%s\",\"rssi\":%d,\"uptime\":%lu,\"heap\":%u}",
         FW_VERSION, WiFi.RSSI(), (unsigned long)(millis() / 1000), ESP.getFreeHeap());
```

The `httpUpdate.update()` return type is the framework enum `t_httpUpdate_return`:
`HTTP_UPDATE_FAILED`, `HTTP_UPDATE_NO_UPDATES`, `HTTP_UPDATE_OK`.

## 5. Protocol / contract

| Topic | Direction | Payload | Retained / QoS | Notes |
|-------|-----------|---------|----------------|-------|
| `home/<id>/ota` | app → device | firmware image URL (an HTTPS URL, e.g. a GitHub release asset — can be ~900 chars) | retained, QoS 0 | Command. The whole payload is the URL, nothing else. |
| `home/<id>/ota/status` | device → app | free-text progress: `starting`, `downloading N%`, `error: no url`, `error <n>: <reason>`, `no update` | retained, QoS 0 | Progress + result. Retained so a late-joining app sees the last known OTA state. |
| `home/<id>/status` | device → all | `online` / `offline` | retained, Last-Will | On successful OTA the device reboots → app sees `offline` (Last-Will) then `online`. |
| `home/<id>/telemetry` | device → app | JSON incl. `"fw"` | retained | After reboot, the bumped `fw` confirms the new image is running. |

Subscription happens in `connectMQTT()` alongside the other command topics:

```cpp
char otaTopic[80];
snprintf(otaTopic, sizeof(otaTopic), "home/%s/ota", DEVICE_ID);
mqtt.subscribe(otaTopic);
```

The status publisher:

```cpp
void publishOtaStatus(const char *msg) {
  char t[80];
  snprintf(t, sizeof(t), "home/%s/ota/status", DEVICE_ID);
  mqtt.publish(t, msg, true);   // retained
  logf("OTA:  %s", msg);
}
```

Cross-repo contract: the app resolves the latest GitHub release, builds/obtains the
`firmware.bin` URL, publishes it (retained) to `home/<id>/ota`, and subscribes to
`home/<id>/ota/status` to render progress. See `HomeControl/docs/features/ota-app.md`.

## 6. Key logic

### Dispatch (`onMessage`)

`onMessage` matches the OTA topic exactly and hands the payload (the URL) straight to
`doOTA`:

```cpp
String t = topic;
if (t == String("home/") + DEVICE_ID + "/ota") {
  doOTA(msg); // payload is the firmware image URL
  return;
}
```

### Forward declaration (required)

`onMessage` is defined **above** `doOTA` in the file, but calls it. So `doOTA` is
forward-declared right before `onMessage`:

```cpp
void doOTA(const String &url); // defined below; used by onMessage
```

Without this the file would not compile — the C++ compiler needs the signature in scope
at the call site. `doOTA` itself is defined further down (after `publishTelemetry` and
`publishOtaStatus`). Keeping the definition low and the declaration high avoids reordering
the whole file.

### The update itself (`doOTA`)

```cpp
// Download + flash a firmware image over HTTPS, then reboot into it. Triggered
// by an MQTT message on home/<id>/ota carrying the image URL. Blocking.
void doOTA(const String &url) {
  if (url.length() < 8) {
    publishOtaStatus("error: no url");
    return;
  }
  logf("OTA:  updating from %s", url.c_str());
  publishOtaStatus("starting");

  WiFiClientSecure otaClient;
  otaClient.setInsecure(); // skip cert check (matches the broker setup)
  otaClient.setTimeout(20000);
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // GitHub redirects
  httpUpdate.onProgress([](int cur, int total) {
    static int last = -1;
    int pct = total > 0 ? (int)((int64_t)cur * 100 / total) : 0;
    if (pct / 25 != last / 25) { // ~every 25%
      last = pct;
      char m[24];
      snprintf(m, sizeof(m), "downloading %d%%", pct);
      publishOtaStatus(m);
    }
  });

  t_httpUpdate_return ret = httpUpdate.update(otaClient, url);
  if (ret == HTTP_UPDATE_FAILED) {
    char m[110];
    snprintf(m, sizeof(m), "error %d: %s", httpUpdate.getLastError(),
             httpUpdate.getLastErrorString().c_str());
    publishOtaStatus(m);
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    publishOtaStatus("no update");
  }
  // HTTP_UPDATE_OK reboots into the new image before returning.
}
```

Line-by-line rationale:

- **`url.length() < 8` guard** — an empty/garbage retained payload (or a cleared topic)
  should not attempt an update. Anything shorter than `https://` can't be a real URL.
- **A fresh `WiFiClientSecure otaClient`** (not the MQTT `net`) — the download is a
  separate TLS connection to a different host (GitHub / its CDN), so it gets its own
  client. It lives on the stack for the duration of the blocking call.
- **`setInsecure()`** — skips certificate-chain validation. This matches the broker setup
  (`net.setInsecure()`), which is acceptable here because the images come from a public,
  known GitHub URL and integrity is enforced by the ESP32 bootloader's image validation
  on the written slot, not by TLS pinning. (A CA bundle could be added later to harden
  this.)
- **`setTimeout(20000)`** — 20 s socket timeout so a stalled connection eventually fails
  cleanly instead of hanging forever.
- **`rebootOnUpdate(true)`** — on a successful write, `httpUpdate` calls `ESP.restart()`
  itself; the function never returns in the success case. That's why there is no explicit
  `publishOtaStatus("ok")` — the app infers success from the reboot (`offline`→`online`)
  and the bumped `fw`.
- **`setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS)`** — **essential for GitHub.** A
  GitHub release asset download (`.../releases/download/<tag>/firmware.bin`) responds with
  an HTTP **302 redirect** to a signed `objects.githubusercontent.com` URL. Without
  forced redirect-following, `httpUpdate` would try to flash the 302 response body and
  fail. `HTTPC_FORCE_FOLLOW_REDIRECTS` follows the redirect even across the GET.
- **`onProgress` throttled to ~25% buckets** — `onProgress` fires very frequently (per
  chunk). Publishing every callback would spam MQTT; the `pct / 25 != last / 25` test only
  publishes when crossing a 25% boundary (0/25/50/75%). `static int last` persists across
  callbacks within the download.
- **Result handling** — `HTTP_UPDATE_FAILED` publishes the numeric error code and the
  framework's error string (e.g. bad HTTP status, not enough space, write error).
  `HTTP_UPDATE_NO_UPDATES` publishes `no update`. `HTTP_UPDATE_OK` is unreachable here
  because the device has already rebooted.

### The MQTT buffer — the key gotcha

In `setup()`:

```cpp
net.setInsecure();          // skip TLS cert validation (fine for a home broker)
mqtt.setBufferSize(2048); // roomy enough for long OTA image URLs
mqtt.setCallback(onMessage);
```

**`mqtt.setBufferSize(2048)` is load-bearing for OTA.** PubSubClient's incoming buffer
defaults to **256 bytes** (`MQTT_MAX_PACKET_SIZE`). If an incoming publish (topic +
payload + framing) exceeds the buffer, **PubSubClient silently drops the whole message**
— no `onMessage` callback, no error, no log line. The command simply vanishes.

The OTA payload is a firmware image URL. GitHub's signed release-asset URLs run **~900
characters** (they carry auth/expiry query parameters). With the earlier 512-byte buffer,
the ~900-char OTA command exceeded the buffer and was silently discarded: the app showed
"update sent", the device logged nothing, and OTA never fired. Bumping the buffer to
**2048** — comfortably above any expected URL length plus topic and MQTT framing overhead
— fixed it. This is the single most confusing failure mode of the whole feature precisely
because it is silent; anyone rebuilding this must set the buffer **before** the first
subscribe and keep it generous.

## 7. Dependencies & setup

- **Libraries (Arduino-ESP32 core, no extra `lib_deps` needed):** `HTTPClient.h`,
  `HTTPUpdate.h` (provides `httpUpdate`), `WiFiClientSecure.h`. `PubSubClient@^2.8`
  (already used for MQTT) must be ≥ 2.7 for `setBufferSize()` to exist.
- **Partition table:** OTA is impossible without two app slots and an `otadata`
  partition. `platformio.ini` sets `board_build.partitions = partitions.csv`; the table
  provides `app0 (ota_0)`, `app1 (ota_1)`, and `otadata`. See `partitions.md`.
- **MQTT buffer:** `mqtt.setBufferSize(2048)` in `setup()` (see §6).
- **No compiled-in secrets:** the image URL is delivered at runtime over MQTT; nothing
  about a specific release is baked into the firmware except `FW_VERSION`.

### Release / hosting process

The firmware repo is **public**, so **GitHub Releases** serve stable, unauthenticated
image URLs — no token, no self-hosted server. To cut a release:

1. Bump `FW_VERSION` in `src/main.cpp` (e.g. `"2.5.0"` → `"2.6.0"`).
2. Build: `~/.platformio/penv/bin/pio run`. The image is at
   `.pio/build/4d_systems_esp32s3_gen4_r8n16/firmware.bin`.
3. Create a GitHub release tagged `fw-<version>` (e.g. `fw-2.6.0`) and attach
   `firmware.bin` as a release asset. Example with the CLI:

   ```bash
   gh release create fw-2.6.0 \
     .pio/build/4d_systems_esp32s3_gen4_r8n16/firmware.bin \
     --title "fw-2.6.0" --notes "OTA test build"
   ```

4. The app resolves the latest `fw-*` release, obtains the `firmware.bin` asset URL, and
   publishes it to `home/<id>/ota`.

Keep the tag scheme `fw-<version>` and the asset name `firmware.bin` stable — the app
relies on both to find the image.

## 8. Edge cases & gotchas

- **Silent buffer drop (the big one):** buffer too small ⇒ the OTA command is dropped
  with zero diagnostics. Symptom: app "sent", device log shows nothing. Fix/guard:
  `setBufferSize(2048)`. See §6.
- **Missing redirect follow:** without `HTTPC_FORCE_FOLLOW_REDIRECTS`, GitHub's 302 makes
  `httpUpdate` fail (it tries to flash the redirect page). Always set it.
- **Empty/garbage URL:** guarded by `url.length() < 8` → `error: no url`. Note the topic
  is retained, so a stale URL can re-trigger on reconnect; the app should clear or
  re-publish deliberately.
- **Insecure TLS:** `setInsecure()` on the OTA client means no cert validation. Acceptable
  because images come from a known public URL and the bootloader validates the written
  image, but it is a deliberate trade-off, not an oversight.
- **Blocking call:** `doOTA()` blocks `loop()` for the whole download (`mqtt.loop()` and
  the heartbeat pause). Fine for an on-demand action; the LWT `status` stays `online`
  until the reboot because we don't publish `offline` before flashing (unlike the
  `restart` command).
- **Failure never bricks the device:** the download writes into the *inactive* slot; only
  after full write + validation does the bootloader flip to it. A failed/interrupted
  download leaves the running slot intact — worst case the device keeps running the old
  firmware and reports an `error` on `ota/status`.
- **Not enough space:** if the image is larger than the OTA slot (`0x300000` = 3 MB),
  `httpUpdate` returns `HTTP_UPDATE_FAILED` with a space error — a reason to keep the app
  well under 3 MB (see `partitions.md`).

## 9. Verification

- **On-device, end-to-end:** verified an over-the-air update from **2.5.0 → 2.6.0**. The
  app published the `fw-2.6.0` `firmware.bin` URL to `home/dev1/ota`; the device logged
  `OTA: starting` → `downloading …%`, rebooted, and came back reporting `"fw":"2.6.0"` in
  telemetry. Confirmed the app saw `home/dev1/status` go `offline` (Last-Will) then
  `online`.
- **Buffer regression reproduced:** with the pre-fix 512-byte buffer, the ~900-char GitHub
  URL produced no `onMessage` and no OTA; raising to 2048 made the same command work —
  which is how the silent-drop root cause was identified.
- **Progress topic:** watched `home/dev1/ota/status` with
  `mosquitto_sub -t 'home/dev1/ota/status' -v` and saw `starting` → `downloading N%`.

## 10. Related

- `HomeControl/docs/features/ota-app.md` — app half: latest-release lookup, URL build,
  publish to `home/<id>/ota`, progress rendering, version compare against `FW_VERSION`.
- `partitions.md` (this repo) — the two-slot OTA partition table OTA requires, and why
  it's migration-safe.
- `telemetry.md` (this repo) — the `home/<id>/telemetry` JSON that carries `fw`, used to
  confirm the running version before/after an update.
- `mqtt-control.md` (this repo) — the base MQTT/TLS transport, topic scheme, and
  retained/Last-Will conventions OTA builds on.
- `device-commands.md` (this repo) — the sibling `restart` / `identify` MQTT commands
  dispatched in the same `onMessage`.
