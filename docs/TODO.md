# Project TODO & Roadmap

Living task tracker for the ESP32-S3 home-automation build. High-level architecture,
hardware, and conventions live in [`../CLAUDE.md`](../CLAUDE.md); this file tracks **what's
done and what's next**.

**Legend:** ✅ done · ⏳ in progress / next · ⬜ not started · ⚠️ safety-critical

> Keep this in sync when work lands: check the box, add a dated line to the **Done log**.

---

## Status at a glance

| # | Stage | Status |
|---|---|---|
| 1 | LEDs over MQTT | ✅ Done (2026-07-04) |
| 2 | Phone control (WebSockets → RN app) | ✅ Done (2026-07-04) |
| 3 | Relays (dry-run via optocouplers) | ⏳ In progress — software done, wiring pending |
| 4 | Power metering (PZEM-004T) | ⬜ Not started |
| 5 | Mains loads (light, fan) | ⬜ Not started ⚠️ |
| — | Housekeeping (secrets, git) | ⬜ Deferred |

---

## Stage 1 — LEDs over MQTT ✅

Done 2026-07-04. Verified end-to-end on hardware (boot → Wi-Fi → MQTT → command round-trip).

- [x] Firmware: 3 LEDs on GPIO 4/5/6 as MQTT channels (`red`/`green`/`blue`)
- [x] MQTT topic contract: `set` / `state` / retained Last-Will `status`
- [x] Retained state + resync-on-connect
- [x] Serial logging with uptime-prefixed tags (boot/WiFi/MQTT/CH/heartbeat)
- [x] Round-trip verified: `red/set on` → LED lit → `red/state on` echoed

## Stage 2 — Phone control ✅

Done 2026-07-04. Bare RN app controls the 3 LEDs from the phone with two-way state sync
verified. (Browser test page was skipped — went straight to the app, which works.)

- [x] Mosquitto WebSockets listener on port `9001` (in `lan.conf`)
- [x] React Native app — **bare RN CLI**, TypeScript (`~/Documents/HomeControl`). Not Expo:
      keeps native modules (TCP sockets, BLE) one `npm install` away for a hardware project.
  - [x] MQTT-over-WebSockets client (`ws://192.168.173.170:9001`) with `buffer` polyfill
        — see `src/useMqtt.ts`
  - [x] Per-channel on/off/toggle controls reflecting retained `state` (two-way sync)
  - [x] Channels driven by `src/config.ts` `CHANNELS` → new firmware channels get UI for free
- [x] Run: `cd ~/Documents/HomeControl && npx react-native run-android` (phone in USB-debug)

## Stage 3 — Relays (dry-run) ⏳ ⚠️  ← next up

Note: adding `relay1..4` to the firmware `channels[]` **and** to `src/config.ts` `CHANNELS`
in the app gives you relay buttons in the phone UI automatically.

Prove relay switching with **nothing on the switched side**.

**Software side is done & verified (2026-07-04):** `relay1..4` on GPIO 15/16/17/18
(`activeLow=true`) are in both `main.cpp` `channels[]` and the app `CHANNELS`; firmware
builds, is flashed to the board, and the full MQTT round-trip (on/off/toggle → retained
`state` echo) was verified with **no hardware wired**. The remaining boxes are purely
physical bring-up at the bench.

- [x] Add `relay1..4` entries to `channels[]` in `main.cpp` — built + flashed + round-trip verified
- [ ] Wire 4-ch relay module via **PC817 optocouplers** on GPIO 15/16/17/18
- [ ] Remove relay board **VCC/JD-VCC jumper** for isolation
- [ ] Confirm active-LOW vs active-HIGH on the actual board; firmware assumes active-LOW —
      invert the channel's `activeLow` if a relay energizes when it should be off
- [ ] Click-test each relay over MQTT (audible click, no load connected)

## Stage 4 — Power metering ⬜

- [ ] Add **PZEM-004T v3.0** (UART, mains-isolated) — **not** ACS712
- [ ] Read voltage/current/power/energy over UART in firmware
- [ ] Publish readings to MQTT (e.g. `home/dev1/power/…`)
- [ ] Pipe into InfluxDB + Grafana for history/dashboards
- [ ] (Clamp meter stays manual-only, for spot-checks)

## Stage 5 — Mains loads ⬜ ⚠️ LETHAL — do last

- [ ] Room light on/off via isolated relay
- [ ] Ceiling fan — **relay = on/off only; speed needs a TRIAC/dimmer, not a relay**
- [ ] Full isolation (optos), enclosure, no exposed conductors
- [ ] Never wire mains while board is USB-powered
- [ ] See safety guardrails in [`../CLAUDE.md`](../CLAUDE.md) §14

---

## Housekeeping / Tech debt ⬜

- [ ] **Move secrets out of `main.cpp`** before the first commit — Wi-Fi password and any
      MQTT creds → gitignored config header (e.g. `include/secrets.h`).
      *(Deferred by owner — must happen before the first commit so the password never
      enters git history.)*
- [x] `git init` firmware repo (branch `main`) — 2026-07-04
- [ ] First commit — **blocked on the secrets move above**
- [ ] Add remote + push
- [ ] Consider broker auth (username/password) instead of anonymous once off the hotspot
- [ ] Per-board `DEVICE_ID` scheme when a second node is added

---

## Done log

- **2026-07-04** — Stage 1 complete: 3 LEDs over MQTT, verified round-trip on hardware.
- **2026-07-04** — Added uptime-prefixed serial logging; verified boot/connect/round-trip/heartbeat on device.
- **2026-07-04** — Authored `CLAUDE.md` project guide and this TODO tracker.
- **2026-07-04** — Mosquitto broker configured on Mac (`1883` TCP + `9001` WebSockets), LAN/anonymous.
- **2026-07-04** — Stage 2 complete: bare RN app (`HomeControl`) controls LEDs over MQTT-WebSockets (`9001`), two-way sync verified; browser test page skipped.
- **2026-07-04** — Firmware repo `git init` (branch `main`); multi-root VS Code workspace (`~/Documents/home-automation.code-workspace`) linking firmware + HomeControl app. Confirmed bare RN CLI (not Expo) is the right fit.
- **2026-07-04** — Stage 3 (software): added `relay1..4` (GPIO 15/16/17/18, active-low) to firmware `channels[]` and app `CHANNELS`; firmware built + flashed; MQTT round-trip (on/off/toggle → retained `state`) verified with no relay hardware wired. Physical wiring/click-test still pending.
