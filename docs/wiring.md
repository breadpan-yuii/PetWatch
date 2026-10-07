# PetWatch — Wiring & Build Sheet (print me)

All pin numbers are for **Arduino UNO R3**. Double-check every connection **before** powering on.

## Powering

- **Power bank** → standard **USB-B cable** → UNO's USB-B port (no barrel jack needed).
- Keep the bank inside the housing so it is "part of the build" (self-powered claim).
- Expected draw: ~80–100 mA idle, more during a servo cycle. Pick a bank that sources ≥ 1 A.

## Connections

### HX711 (load cell amplifier) — Stage 2

| HX711 | Arduino UNO |
|-------|-------------|
| VCC   | 5V          |
| GND   | GND         |
| DOUT  | D3          |
| SCK   | D2          |

Load cell → HX711: **red → E+**, **black → E−**, **white → A−**, **green → A+** (varies by cell — verify; calibration compensates anyway).

### RTC (DS3231) — Stage 1

| RTC    | Arduino UNO |
|--------|-------------|
| VCC    | 5V          |
| GND    | GND         |
| SDA    | A4          |
| SCL    | A5          |

Fit the **CR2032** so time survives power-off.

### LCD (16×2 I2C) — Stage 1

| LCD | Arduino UNO |
|-----|-------------|
| VCC | 5V          |
| GND | GND         |
| SDA | A4 (shared with RTC) |
| SCL | A5 (shared with RTC) |

### Servo — Stage 3

| Servo  | Arduino UNO |
|--------|-------------|
| Signal (orange/yellow) | D9 |
| Power  (red) | 5V |
| Ground (brown) | GND |

### Buzzer (active, 5 V) — Stage 1

| Buzzer | Arduino UNO |
|--------|-------------|
| (+)    | D8          |
| (−)    | GND         |

## Assembly order

1. **Stage 1** — RTC + LCD on A4/A5, buzzer on D8. Run `test/01_display_rtc.ino`. LCD shows time; Serial Monitor (9600) shows RTC ok.
2. **Stage 2** — HX711 on D2/D3. Run `test/02_scale.ino` for a reading; then in the main sketch: `tare` (empty bowl on platform), `cal 100` (100 g weight).
3. **Stage 3** — Servo on D9, flap on hopper outlet over the bowl. Run `test/03_servo.ino` to confirm sweep; then `feed` from Serial Monitor and tune `GRAMS_PER_CYCLE` in the main sketch so each cycle ≈ 10 g.
4. **Stage 4** — mount load cell → stiff platform → bowl; hopper over bowl; house everything with the power bank; run the checklist in `docs/build-log.md`.

## Tips

- LCD blank? Try address `0x3F` instead of `0x27` in the sketch (`LCD_ADDR`).
- **RTC wedges the bus?** If boot alternates between a clock and `ERR,rtc_not_found` (all I2C addresses
  time out together), the DS3231 module itself is unreliable — reseating/power-cycling is the only
  recovery. The sketch self-heals via `ensureRTC()` but for demo day use a **known-good spare module**
  (the shipped one has no usable onboard pull-ups and intermittently holds the bus low).
- Servo twitching unnervingly at boot is normal — the sketch parks it closed at 15°.
- If the UNO resets when dispensing, the bank can't surge — use a ≥2 A bank or feed the servo from a separate 5 V source with common ground.