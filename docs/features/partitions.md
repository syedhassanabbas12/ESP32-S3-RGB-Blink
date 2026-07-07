# OTA Partition Table (16 MB flash)

> **Status:** shipped · **Repo(s):** firmware · **Since:** `partitions.csv` + `platformio.ini` (`board_build.partitions`)

Recreation-grade spec: someone (or Claude) with only this document should be able to
rebuild this feature identically. This documents the custom flash partition table that
makes over-the-air updates (see `ota.md`) physically possible, and — just as importantly —
why it is laid out to be a **safe migration** from the stock `huge_app` table.

## 1. Summary

`partitions.csv` is a custom ESP32 partition table for the board's **16 MB flash** that
gives the firmware **two application slots** (`ota_0` / `ota_1`) plus an `otadata`
partition, which is the minimum an ESP32 needs to update itself over the air. The layout
is deliberately chosen so that `nvs`, `otadata`, and `app0` sit at the **exact same
offsets** as the stock Arduino `huge_app.csv` table. That means migrating a board from
`huge_app` to this table (a) preserves everything saved in `nvs` — the MQTT broker
config, device id, WiFi credentials — and (b) leaves the already-running firmware in
`app0` untouched. Only the previously-unused tail of flash is repartitioned to carve out
the second app slot. It's wired into the build via
`board_build.partitions = partitions.csv` in `platformio.ini`.

## 2. User-facing behavior

There is no direct user-facing surface — a partition table is a build-time artifact. Its
observable effects:

- **OTA works at all.** With a single-app table (the plain default), `httpUpdate` has
  nowhere to write the new image and OTA fails. With this table it succeeds (see `ota.md`).
- **Config survives the migration.** When the device was first flashed with this table
  over USB, it came back up with its saved broker host/port/user/pass and device id intact
  — no re-provisioning needed — because `nvs` stayed at the same offset.
- **App size ceiling.** Each firmware image must fit in one 3 MB app slot. If the build
  ever exceeds `0x300000`, `pio run` fails to link/upload and OTA reports a space error.

## 3. Architecture & files

| File | Role |
|------|------|
| `partitions.csv` | The partition table itself — six partitions covering the full 16 MB. |
| `platformio.ini` | `board_build.partitions = partitions.csv` tells PlatformIO/esptool to use this table instead of the board's default. |
| ESP-IDF 2nd-stage bootloader (built in) | Reads `otadata` at boot to decide which of `ota_0` / `ota_1` to run; validates the selected image before jumping to it. |

`platformio.ini` wiring (with its explanatory comment):

```ini
[env:4d_systems_esp32s3_gen4_r8n16]
platform = espressif32
board = 4d_systems_esp32s3_gen4_r8n16
framework = arduino
monitor_speed = 115200
; OTA-capable table (two app slots) that preserves nvs + app0 from huge_app,
; so migrating keeps saved config and the running firmware.
board_build.partitions = partitions.csv
lib_deps =
    knolleary/PubSubClient@^2.8
```

## 4. Data model / types

The partition table is CSV with columns `Name, Type, SubType, Offset, Size`. The exact
file:

```csv
# OTA-capable partition table for 16MB flash.
# nvs + otadata + app0 keep the same offsets as huge_app.csv, so migrating from
# huge_app preserves the saved config (nvs) and the running firmware (app0).
# Adds a second app slot (ota_1) so the device can update over the air.
# Name,     Type, SubType,  Offset,    Size
nvs,        data, nvs,      0x9000,    0x5000
otadata,    data, ota,      0xe000,    0x2000
app0,       app,  ota_0,    0x10000,   0x300000
app1,       app,  ota_1,    0x310000,  0x300000
spiffs,     data, spiffs,   0x610000,  0x9E0000
coredump,   data, coredump, 0xFF0000,  0x10000
```

Decoded, with human-readable sizes and the end offset of each region:

| Name | Type | SubType | Offset | Size | Size (human) | Ends at | Purpose |
|------|------|---------|--------|------|--------------|---------|---------|
| `nvs` | data | nvs | `0x9000` | `0x5000` | 20 KB | `0xE000` | Non-volatile key/value store. Holds the `homecfg` namespace (broker host/port/user/pass, `devid`, `forceprov`) **and** the WiFi credentials saved by provisioning. |
| `otadata` | data | ota | `0xe000` | `0x2000` | 8 KB | `0x10000` | Two-copy record of which app slot is active/bootable. The bootloader reads this to pick `ota_0` vs `ota_1`. |
| `app0` | app | ota_0 | `0x10000` | `0x300000` | 3 MB | `0x310000` | First application slot. The firmware flashed over USB lands here; the running image after a fresh flash. |
| `app1` | app | ota_1 | `0x310000` | `0x300000` | 3 MB | `0x610000` | Second application slot. OTA writes the new image here (then `otadata` flips to it). |
| `spiffs` | data | spiffs | `0x610000` | `0x9E0000` | ≈ 9.875 MB | `0xFF0000` | Filesystem partition (unused today, reserved for future on-device storage). |
| `coredump` | data | coredump | `0xFF0000` | `0x10000` | 64 KB | `0x1000000` | Crash core-dump region for post-mortem debugging. |

The region below `nvs` (offsets `0x0`–`0x9000`) is the standard ESP32 reserve: the
bootloader lives at `0x0` and the partition table itself at `0x8000`. Nothing in the CSV
touches it. The table fills flash exactly: `0x1000000` = 16 MB.

## 5. Protocol / contract

No MQTT/network contract. The contract here is between the **build** and the **bootloader**:

- OTA requires **≥ 2 partitions of type `app`** with subtypes `ota_0` and `ota_1`, plus a
  `data`/`ota` partition named `otadata`. This table provides exactly that.
- The bootloader's rule: on boot it reads `otadata`, selects the marked slot, validates
  its image, and runs it (falling back to the other slot if validation fails). `httpUpdate`
  writes the *inactive* slot and, on a verified full write, updates `otadata` to point at
  it — which is what makes a failed OTA non-bricking (the old slot stays selected until the
  new one is proven).
- App images must be ≤ the app-slot size (`0x300000` = 3 MB).

## 6. Key logic — the migration-safety rationale

This is the crux of the design. The obvious way to get OTA is to pick any two-app table.
Instead this table is **derived from `huge_app.csv`** by keeping its head identical and
splitting only its tail.

Stock Arduino `huge_app.csv` (a single-app, 3 MB layout) is:

```
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
app0,     app,  ota_0,   0x10000, 0x300000
spiffs,   data, spiffs,  0x310000, ...        <- everything after app0 was spiffs
```

The board had been running on `huge_app` (or an offset-compatible layout), so all saved
state lived in `nvs @ 0x9000` and the live firmware in `app0 @ 0x10000`. Two invariants
had to hold across the switch to an OTA table:

1. **Preserve saved config.** `nvs` holds the broker credentials, device id, and WiFi
   credentials. If `nvs` moved even one byte, the NVS partition would be re-initialized
   and the device would boot unconfigured — needing a full re-provision over BLE/serial.
   → **`nvs` is kept at `0x9000 / 0x5000`, byte-identical to `huge_app`.**
2. **Preserve the running firmware.** `app0` holds the image currently executing. Keeping
   `app0` at the same offset and size means the migration doesn't have to move or re-flash
   the running code; it's still valid and bootable.
   → **`app0` (and `otadata`) kept at their `huge_app` offsets.**

The **only** change from `huge_app` is what happens *after* `app0`: where `huge_app` put
`spiffs`, this table inserts a second 3 MB app slot (`app1 / ota_1` at `0x310000`), then
places `spiffs` and `coredump` in the remaining flash. Because the 16 MB chip has ~13 MB
free past `app0`, there's ample room to add the 3 MB second slot and still leave ~9.9 MB
for spiffs.

Rejected alternatives and why:

- **A generic dual-app OTA table (e.g. the stock `default.csv` / `min_spiffs`)** — these
  place `nvs`/`app0` at compatible offsets but size the app slots differently (often
  1.25–1.9 MB), which would *shrink* the app slot below the current image and could
  reshuffle offsets. The hand-rolled table guarantees 3 MB slots and identical head
  offsets. Rejected the stock tables in favor of an explicit CSV.
- **Moving/enlarging `nvs`** — would wipe saved config. Not worth it; 20 KB is plenty.
- **A single giant app + OTA via SPIFFS staging** — more complex, no bootloader-level
  A/B safety. Rejected in favor of native two-slot OTA.
- **Two app slots > 3 MB each** — unnecessary; the firmware is far under 3 MB, and larger
  slots would eat the spiffs/coredump space for no benefit.

## 7. Dependencies & setup

- **`platformio.ini`:** `board_build.partitions = partitions.csv` (relative to the project
  root). Without this line PlatformIO uses the board default and this file is ignored.
- **Board:** `4d_systems_esp32s3_gen4_r8n16` — an ESP32-S3 with **16 MB flash** (N16R8).
  The table's total (`0x1000000`) assumes exactly 16 MB; on a smaller chip the offsets
  would overflow.
- **Toolchain:** esptool (invoked by PlatformIO) flashes the table at `0x8000` as part of
  a normal `pio run -t upload`.
- **No code changes** are needed to consume the table — the Arduino `Update`/`httpUpdate`
  machinery discovers the `ota_*` partitions at runtime via the partition table.

## 8. Edge cases & gotchas

- **Offsets are contiguous and must stay so.** Each partition's `offset + size` equals the
  next partition's offset; the table ends exactly at `0x1000000`. Editing one size without
  fixing the following offset will overlap partitions or leave gaps → flash/boot failure.
- **`nvs` offset is sacred.** Moving it silently discards all saved config on next boot.
  Any future table change must keep `nvs @ 0x9000 / 0x5000` to remain migration-safe.
- **App must fit 3 MB.** Exceeding `0x300000` fails the build/upload; OTA of an oversized
  image fails with a space error (see `ota.md` §8).
- **Alignment:** app partitions must start on a 0x10000 (64 KB) boundary — `0x10000` and
  `0x310000` both satisfy this. Don't hand-edit an app offset to a non-aligned value.
- **Flashing this the first time is a USB operation, not OTA.** You cannot OTA *onto* a
  table that has no second slot — the first flash of this table went over USB.

## 9. Verification

- **Migration verified on hardware:** the table was flashed **over USB** with the ESP32 as
  a recovery net (USB always available to reflash if anything went wrong). After the
  switch, the device booted straight into its previous firmware and **its saved config
  survived** — broker host/port/credentials and device id were intact, confirming `nvs`
  and `app0` stayed put across the migration. No re-provisioning was required.
- **OTA proven on this table:** an over-the-air update **2.5.0 → 2.6.0** succeeded end to
  end (see `ota.md` §9), which exercises writing `ota_1`, flipping `otadata`, and booting
  the new slot — i.e. proof the two-slot table is functioning.
- **Offset arithmetic checked:** every `offset + size` chains to the next offset and the
  table sums to exactly 16 MB (see §4 "Ends at" column).

## 10. Related

- `ota.md` (this repo) — the HTTPS OTA feature that this partition table exists to enable;
  covers `httpUpdate`, the MQTT buffer gotcha, and the release process.
- `wifi-broker-config.md` (this repo) — what actually lives in the `nvs` partition
  (`homecfg` namespace + WiFi credentials) that the migration was careful to preserve.
- `ble-provisioning.md` (this repo) — the re-provisioning path you'd be forced back into if
  `nvs` were ever wiped by a bad partition change.
- `HomeControl/docs/features/ota-app.md` — app-side counterpart of the OTA flow.
