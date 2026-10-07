# PetWatch: Automated Pet Feeder with Adaptive Schedules

An Arduino-based automatic feeder that dispenses food at set times using a real-time clock and monitors the bowl with a weight sensor. It learns the pet's eating habits over time — auto-adjusting portion sizes and feeding times — and logs every meal on an LCD for appetite and health tracking.

---

## 1. Materials List

> **Libraries to install first** (Sketch > Include Library > Manage Libraries...):
> - **LiquidCrystal_I2C** (Frank de Brabander) — search "LiquidCrystal I2C"
> - **RTClib** (Adafruit) — search "RTClib"
> - **HX711** (Bogdan Necula) — search "HX711"

### Electronics

| # | Component | Qty | Spec / Notes | Alternative |
|---|-----------|-----|--------------|-------------|
| 1 | Arduino UNO R3 (or clone) | 1 | ATmega328P | UNO R4 (pins differ slightly) |
| 2 | Load cell + HX711 amplifier module | 1 | 1 kg (recommended for pet food) | 5 kg for larger bowls |
| 3 | Real-Time Clock (RTC) module | 1 | DS3231 (more accurate, must include CR2032 battery holder) | DS1307 |
| 4 | Servo motor | 1 | MG996R (metal gear, good torque for a hopper flap) | SG90 / MG90S for a light cardboard hopper |
| 5 | 16x2 LCD with I2C adapter | 1 | I2C version (only 2 data pins) | Parallel 16x2 LCD (more pins + contrast pot) |
| 6 | Active buzzer | 1 | 5 V active (beeps with logic HIGH) | Passive buzzer (needs tone() ) |
| 7 | CR2032 coin battery | 1 | For the RTC (bought separately) | Included with some DS3231 modules |
| 8 | Jumpers (M/F, M/M) | ~25 | Breadboard + wiring | Dupont cables |
| 9 | Breadboard + base plate | 1 | Half-size or full-size | Perfboard (solder final version) |

### Mechanical / Housing

| # | Part | Qty | Notes |
|---|------|-----|-------|
| 10 | Food hopper (cardboard/plastic bottle/3D print) | 1 | Funnel shape; servo rotates a flap at the bottom to release food |
| 11 | Pet bowl (non-slip) | 1 | Sits on the load cell |
| 12 | Platform for load cell | 1 | Stiff board the bowl rests on (load cell flexes under weight) |
| 13 | Enclosure/diorama housing | 1 | For the electronics — any project box or laser-cut/painted panel |

### Power (self-powered — required for the defense)

> **This build uses a USB power bank** (no AA pack). The bank powers the UNO via its
> USB-B port. It must be part of the build (taped in the housing), NOT the class's outlet.
> idles at ~80–100 mA, well above auto-sleep cutoffs; pick a bank that sources ≥1 A for the servo.

| # | Part | Qty | Notes |
|---|------|-----|-------|
| 14 | USB power bank (primary) | 1 | USB cable into the UNO USB-B port; part of the build |
| 15 | 6x AA battery pack (9 V) | 1 | Alt: barrel jack (7–12 V OK) if the bank is not available |
| 16 | AA batteries | 6 | Rechargeable (NiMH) preferred, for the pack alt |
| 17 | External 5 V for the servo (optional) | 1 | 4x AA pack (6 V) + shared ground, only if the servo browns out |

---

## 2. Wiring Guide

> Pin numbers are for Arduino UNO R3. Double-check every connection before powering on.

### HX711 (load cell amplifier)

| HX711 | Arduino UNO |
|-------|-------------|
| VCC | 5V |
| GND | GND |
| DOUT | D3 |
| SCK | D2 |

Load cell wires to HX711: red → E+, black → E-, white → A-, green → A+ (color order varies by load cell; calibrate in code).

### RTC (DS3231)

| RTC | Arduino UNO |
|-----|-------------|
| VCC | 5V |
| GND | GND |
| SDA | A4 |
| SCL | A5 |

### LCD (16x2 I2C)

| LCD | Arduino UNO |
|-----|-------------|
| VCC | 5V |
| GND | GND |
| SDA | A4 (shared with RTC) |
| SCL | A5 (shared with RTC) |

### Servo

| Servo | Arduino UNO |
|-------|-------------|
| Signal (orange/yellow) | D9 |
| Power (red) | 5V |
| Ground (brown) | GND |

### Buzzer

| Buzzer | Arduino UNO |
|--------|-------------|
| (+) | D8 |
| (−) | GND |

---

## 3. Assembly Order (suggested)

1. Wire RTC + LCD + buzzer on the breadboard — verify the clock time and LCD display text with a small test sketch.
2. Wire the HX711 + load cell — run the calibration step (see code) and confirm stable gram readings with empty vs full bowl.
3. Mount the load cell on the base, platform on top, bowl on the platform.
4. Build the hopper (funnel), attach the servo to the flap/outlet, position it over the bowl.
5. Assemble everything in the housing/diorama; plug in the battery pack.

## 4. Power Notes

- UNO barrel jack: 6x AA pack (9 V) is correct — the UNO's onboard regulator handles 7–12 V.
- If the MG996R servo stalls or the UNO resets when dispensing, feed the servo from a separate 6 V pack and connect only its GND and signal to the UNO (common ground).
- The RTC needs its CR2032 battery so it keeps time when the main power is off.

## 5. Configuration

Editable at the top of `petwatch.ino` under "USER-TUNABLE SETTINGS".

| Setting | Default | Where |
|---------|---------|-------|
| Feeding times | 08:00 / 18:00 | `MEAL_A_HOUR/MIN`, `MEAL_B_HOUR/MIN` |
| Portion size | 20 g | `DEFAULT_PORTION` |
| Grams per dispense cycle | 10 g | `GRAMS_PER_CYCLE` (tune to your hopper) |
| Minimum / max portion | 5 / 80 g | `MIN_PORTION` / `MAX_PORTION` |
| "Not eating" alert | 3 h | `NOT_EATING_MIN` |
| Learning window | 5 feedings | `ADAPT_WINDOW` |
| Portion step | 15 % | `PORTION_STEP` |
| Schedule shift | 15 min | `SCHEDULE_STEP` / `SHIFT_MARGIN` |
| Load cell calibration | run `cal` in Serial | stored to EEPROM |

## 5b. Serial Commands (Serial Monitor, 9600 baud)

> **Headless mode:** the LCD is installed — `USE_LCD 1` (top of `petwatch.ino`) drives the
> I2C display at `0x27`. Set it to `0` to run with no LCD attached: the two display lines then
> stream to the Serial Monitor as `LCD:...` every 2 s.

> **Set the clock for a demo:** plug the feeder into any PC (Linux/Windows/macOS) and run
> `python3 tools/rtc_sync.py` — it pushes the computer's time to the RTC via `settime` and
> exits. The DS3231 then keeps the time by itself on the power bank. No installs needed:
> stock Python 3 (pyserial is used only if already present).

| Command | Purpose |
|---------|---------|
| `tare` | Zero the scale with the empty bowl in place |
| `cal 100` | With a known 100 g weight on the bowl, sets the scale factor (stored to EEPROM) |
| `scal` | Print `cal=` / `raw=` / `grams=` together to check accuracy without recalibrating |
| `settime 2026 9 18 8 0 0` | Set the RTC date/time (y m d hh mm ss) |
| `feed` | Force a feeding cycle now (for demos) |
| `reset` | Restore factory settings |
| `status` | Print portion, meal times, calibration, servo range, bowl weight |

### Servo calibration

Stock 544–2400 µs drives many servos into their end stops, where they stall,
overheat and strip the horn. Trim the real range once, then `ssave` it to EEPROM.

| Command | Purpose |
|---------|---------|
| `srange` | Show the current range, park angle and live pulse width |
| `ssweep` | Slow sweep between the calibrated ends (any other command stops it) |
| `pulse <us>` | Drive one raw pulse width (400–2600) |
| `creep <±us>` | Nudge the live pulse width by a delta — fine-tune near a stop |
| `smin <us>` / `smax <us>` | Set the calibrated end, then re-attach with it |
| `spark <deg>` | Set the angle the flap parks at on boot |
| `ssave` | Persist range + park angle to EEPROM (re-boot to confirm) |
| `soff` / `son` | Detach (drops holding torque) / re-attach |

`SERVO_OPEN`/`SERVO_CLOSED` stay in **degrees** (95/15); the calibrated pulse
range maps those degrees onto whatever the fitted servo actually tracks.

## 6. Repository Layout (planned)

```
PetWatch/
├── README.md            ← this file
├── petwatch.ino         ← main sketch (code)
├── test/                ← per-stage test sketches (upload each sub-sketch to verify one module)
│   ├── 01_display_rtc.ino
│   ├── 02_scale.ino
│   └── 03_servo.ino
├── docs/
│   ├── wiring.md        ← print-ready wiring/assembly sheet
│   └── build-log.md     ← stage verification checklist (progress evidence)
├── tools/
│   ├── rtc_sync.py      ← push the PC's clock to the RTC before a demo (no deps)
│   └── petwatch_gateway.py ← serial→webhook IoT gateway
└── assets/
    ├── expected-outcome.png   ← build diagram
    └── (photos of the build)
```

## 7. TODO / Verification Checklist

- [ ] LCD displays current time and bowl weight
- [ ] RTC keeps correct time after power-off (CR2032 installed)
- [ ] Load cell calibrated — grams match a known-weight food amount
- [ ] Servo dispenses exactly the set portion
- [ ] Servo end stops calibrated and saved (`ssave`) — horn does not heat up
- [ ] Missed-meal alert buzzes when weight unchanged past timeout
- [ ] Meal log survives power-off (EEPROM)
- [ ] Runs entirely on the battery pack (no wall power)
- [ ] Housed in finished enclosure/diorama

## 8. Building / verifying

No Arduino IDE is required to check the sketch compiles:

```bash
arduino-cli compile --fqbn arduino:avr:uno petwatch/
```

Expected with the LCD installed (`USE_LCD 1`): ~72% flash, ~51% RAM. Headless
(`USE_LCD 0`): a few bytes less. Serial literals use `F()` so they stay out of
RAM; keep new ones that way. See the bug sweep in `docs/build-log.md`.