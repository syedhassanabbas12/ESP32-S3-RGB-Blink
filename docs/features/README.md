# ESP32-S3 Firmware — Feature Docs

Recreation-grade documentation, one file per feature. Follow the template used in the
app repo (`HomeControl/docs/features/_TEMPLATE.md`). Every new feature is documented in
the same commit; every change updates its doc. App-side counterparts live in the
`HomeControl` repo under `docs/features/`.

## Index (firmware)

| Doc | Feature |
|-----|---------|
| [channels-gpio.md](channels-gpio.md) | Channel model, GPIO map, active-low relays, pin drive |
| [mqtt-control.md](mqtt-control.md) | Topic scheme, connect, subscribe, set/state, Last-Will |
| [wifi-broker-config.md](wifi-broker-config.md) | NVS-persisted config + serial `set`/`wifi` provisioning |
| [ble-provisioning.md](ble-provisioning.md) | WiFiProv BLE provisioning (+ serial fallback) — pairs with app `ble-provisioning-app.md` |
| [telemetry.md](telemetry.md) | `home/<id>/telemetry` JSON (fw/rssi/uptime/heap) — pairs with app device-detail-sheet |
| [device-commands.md](device-commands.md) | `restart` + `identify` MQTT commands |
| [ota.md](ota.md) | HTTPS OTA (httpUpdate), buffer sizing, release process — pairs with app `ota-app.md` |
| [partitions.md](partitions.md) | OTA partition table + migration safety (nvs/app0 preserved) |
