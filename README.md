# HomeControl — ESP32-S3 Firmware

Arduino/PlatformIO firmware for a 4D Systems ESP32-S3 controller: drives RGB LEDs +
relays, talks MQTT/TLS to HiveMQ Cloud, and speaks the HomeControl topic contract
(control, telemetry, BLE provisioning, OTA, identify, restart). No secrets are
compiled in — broker + device id are provisioned at runtime and saved to NVS.

> 📖 **Flashing this from scratch? → [SETUP.md](SETUP.md)** (PlatformIO, upload,
> provisioning, broker config, OTA). System bring-up: [../../../HomeControl/SETUP.md](../../../HomeControl/SETUP.md).

- **Feature docs (recreation-grade):** [docs/features/README.md](docs/features/README.md)
- **Project conventions / architecture:** [CLAUDE.md](CLAUDE.md)
- Part of the HomeControl system: `HomeControl` (app) · `homecontrol-server` (24/7 engine) · this repo (firmware).
