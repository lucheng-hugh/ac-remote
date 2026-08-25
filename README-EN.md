# ESP32-S3 Gree Air Conditioner Remote

ESP32-S3 based smart remote for Gree air conditioners. Features:

- Local web control from a phone
- JSON API control
- Weekly repeating schedules (stored in NVS)
- Sync web UI state when receiving YAP0F21 original remote commands
- Sleep modes: off / 1 / 2 / 3 / 4 (accessible via web UI, API, schedules and remote sync)
- Native HomeKit / Siri control with two-way state synchronization
- ESP32 actively probes configured iPhone IPs to auto-turn off when away and auto-turn on when returning
- Unified setpoint range across all control interfaces: 16–28°C
- First-time setup uses the device hotspot for Wi‑Fi onboarding; Wi‑Fi password is not hard-coded

## Wiring

- Receiver: OUT -> GPIO4, VCC -> 3V3, GND -> GND
- Transmitter: DAT/OUT -> GPIO5, VCC -> 5Vin, GND -> GND

## Local commands

```sh
.venv/bin/pio run
.venv/bin/pio run --target upload
.venv/bin/pio device monitor
```

## First-time network setup

1. Connect to Wi‑Fi network `AC-Remote-Setup` (password `acremote123`).
2. Open a browser at `http://192.168.4.1` and enter your home 2.4GHz Wi‑Fi credentials.
3. After the device reboots, visit `http://ac-remote.local` or use the LAN IP shown on the serial console.

## API

- `GET /api/state`: read the air conditioner state
- `POST /api/control`: send control commands to the air conditioner
- `GET /api/schedules`: read scheduled tasks
- `POST /api/schedules`: add or update a schedule
- `DELETE /api/schedules?id=1`: delete a schedule
- `GET|POST /api/time`: read or set the device time
- `GET|POST /api/homekit`: read HomeKit status or set the pairing code from the device web UI
- `GET|POST /api/presence`: read or configure iPhone presence detection

`POST /api/control` accepts `"sleepMode": 0..4` to set sleep mode. The legacy `"sleep": true/false` field is still supported and maps to sleep mode 1 / off.

## Auto-away (turn off) / Auto-home (turn on)

No Apple TV, HomePod or router scripts required. You can configure up to four phones; the ESP32 actively probes their fixed LAN IPs every 15 seconds. If any phone responds to two consecutive probes, the system considers someone "home". Only when all configured phones remain unreachable for the configured away delay will the system consider the house empty and turn off the AC. The default away delay is 10 minutes to avoid false off events due to brief disconnections or locked phones.

1. Reserve fixed LAN IPs for each iPhone in your router (DHCP/static lease).
2. On each iPhone, open `http://ac-remote.local` in a browser.
3. In the "Auto-away / Auto-home" section, click "Add current phone" and save, then repeat on the next phone.
4. The page shows the scheduled turn-off time and remaining countdown in real time. When first enabled (or after an ESP32 reboot), the device will not auto-turn-on even if phones are present; if all phones go offline the countdown will finish and the device will turn off.

This method depends on iPhones responding to LAN probes (ICMP). Router and iOS power-saving behaviors vary; we recommend starting with a 10–15 minute away delay for observation. If false triggers persist, checking the router's Wi‑Fi client list is usually more reliable than active probing from the ESP32.

## HomeKit / Siri

Set an 8-digit HomeKit pairing code from the bottom of the device web UI. Then add the accessory on iPhone via Home app: Add Accessory → More Options → Gree Air Conditioner. Cooling/heating/auto, target temperature, fan speed, swing, dehumidify, fan-only and sleep modes 1–4 are exposed as services under the same AC accessory and share the same room. State from the original remote will sync back to the Home app.

The current hardware does not include an ambient temperature sensor, so HomeKit's "Current Temperature" is temporarily shown as the configured setpoint.
