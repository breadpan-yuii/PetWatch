# PetWatch — Build Log & Verification

Keep this updated as you go. It's your **progress evidence** for the defense.
Mark with `[x]` when done. Fill a line per stage with the Serial output observed.

## Stage 0 — Toolchain (done by automation, verify once)
- [ ] Arduino IDE opens `~/Arduino/PetWatch/PetWatch.ino` (symlink → repo)
- [ ] Sketch compiles clean in the IDE (Upload button turns orange)

## Stage 1 — RTC + LCD + buzzer
- [ ] Upload `test/01_display_rtc.ino`
- [ ] **Headless (no LCD yet):** set `USE_LCD 0` in `petwatch.ino` — Serial Monitor shows `LCD:hh:mm:ss  WWg` lines every 2 s instead
- [ ] LCD shows "RTC FOUND" / "RTC FAIL", clock ticks on line 2 *(skip until the I2C LCD arrives)*
- [x] **Boot-freeze fixed (2026-10-04):** missing HX711 hung boot via `scale.begin()`→`reset()`→`read()`
      (`doReset=false` now); `Wire.setWireTimeout` added so a stuck I2C can't freeze boot/loop
- [ ] **RTC module is defective (2026-10-05):** DS3231 intermittently wedges the whole I2C bus (all
      addresses time out) and only recovers on power-cycle/reseat. It also ships without usable onboard
      pull-ups (needs the UNO's internal ones: `digitalWrite(SDA,1)/digitalWrite(SCL,1)`). Proven clean
      runs (20× ACK over 15 s) alternate with total bus wedges with identical wiring/code.
      **Action: order a backup DS3231 module before demo day.** Firmware now self-heals: warm-up probe +
      retry at boot, then `ensureRTC()` keeps retrying in loop; clock shows `--:--` while the RTC is out
      (feeder still boots/feeds-safe on command).
- [ ] Serial shows a found I2C address `0x68` (RTC) and LCD address `0x27`/`0x3F`
- [x] **I2C bus dead + blank LCD root-caused (2026-10-06).** The LCD arrived and stayed blank.
      Scanner sketch: `SDA=0 SCL=1`, 126/126 addresses timed out, 9-clock SCL recovery failed
      → hard short, not a software fault. Unplugging the LCD changed nothing; isolating
      wire-by-wire found **SDA and SCL swapped at the DS3231 module**. With the swap corrected
      the bus reads `SDA=1 SCL=1` and ACKs **0x27 (LCD)**, **0x57** (module EEPROM),
      **0x68 (RTC)** — so the scanner item above is confirmed. The LCD was innocent; likely the
      same root cause as the 2026-10-05 "defective module" note, so re-verify long-run
      stability before ordering a spare. Recipe: `SDA=0 + SCL=1 + every address times out`
      = bus shorted low → pull devices one at a time to find the culprit connection.
- [x] **LCD enabled in the main sketch (2026-10-06):** `USE_LCD 1`, `LCD_ADDR 0x27`
      (confirmed by scan), build **69% flash / 81% RAM**. Display goes to the LCD;
      Serial now carries commands + boot log only.
- [ ] `settime 2026 10 7 9 0 0` in Serial → prints `OK,time set` and buzzer blips
- [ ] 3 boot beeps heard
- Note: _____

## Stage 2 — Load cell + HX711
- [ ] **Module in hand as of 2026-10-05 — unblocked, ready to verify.**
- [ ] Upload `test/02_scale.ino`
- [ ] raw value changes when you press on the bowl platform
- [ ] `z` zeros; add known weight, `c 100` → grams≈100; remove weight → grams≈0
- [ ] Re-upload main `petwatch.ino`; `tare`, then `cal 100` → `OK,cal=...`
- [ ] `status` shows `calSet=yes` and a sane `cal=` / `bowl=`
- [ ] `scal` prints `cal=` / `raw=` / `grams=` together to sanity-check
- Note (load cell wire colors actually used): _____

## Stage 3 — Servo + hopper flap
- [ ] Flash `test/03_servo.ino`; horn sweeps open/closed via Serial ('o'/'c')
- [ ] **BLOCKED (hardware, 2026-10-05):** only 3 wires were tested (D9, 5V, GND); pulses
      echoed (servo=95/15) but the horn does not move and gets hot — seized/stripped.
      Replacement SG90 still required.
- [ ] **Still blocked (2026-10-06):** SG90/MG90S fitted, flashed `test/03_servo.ino`;
      `o`/`c` echo `servo=95/15` but the horn still does not move. Outstanding checks:
      (1) does the motor get warm — warm = seized/stripped again, cold = no power/no signal;
      (2) feed 5 V straight from the UNO 5V/GND pins instead of the breadboard rail
      (half-size rails are split mid-board — a dead segment gives silent echoes);
      (3) confirm the signal wire is on D9 and the servo snaps to 15° at sketch attach.
- [ ] Once a servo is fitted, trim its real end stops in the main sketch:
      `ssweep` → creep with `creep ±10` / `pulse <us>` → set `smin <us>` and `smax <us>`
      just inside the stops → `spark 15` → `ssave`. Re-boot and confirm `status`
      reports the saved range.
- [ ] `feed` drops food into bowl
- [ ] `GRAMS_PER_CYCLE` tuned so one flap cycle ≈ 10 g; actual: ___ g
- Note: firmware drives SERVO_OPEN=95 / SERVO_CLOSED=15 in degrees; the calibrated
      pulse range maps those degrees onto whatever the fitted servo actually tracks.

## Stage 4 — Full system (battery-only dry run)
- [ ] Runs from the power bank, **no wall/USB programming cable during demo**
- [ ] LCD row1 shows `<time>  <grams>`
- [ ] RTC keeps time after unplugging power 5 min (CR2032)
- [ ] **RTC synced from PC before the power-bank run:** plug into PC →
      `python3 tools/rtc_sync.py` → `OK,time set`; unplug and the clock keeps ticking
- [ ] `feed` → food falls, screen "FED Xg", 2 beeps
- [ ] Portion matches setting (±3 g)
- [ ] Bowl empty → "BOWL EMPTY" line
- [ ] Meal log survives `reset` demo: `status` shows entries after a power cycle
- [ ] Housing assembled; power bank inside = self-powered
- [ ] **Dry-run**: `settime` to just-before meal time, watch auto-feed fire
- Photos: [ ] hopper [ ] wiring [ ] assembled [ ] LCD-lit

## Demo-day script (things to narrate)
1. Battery only — unplug any cable live.
2. `status` → show portion/meals/calibration.
3. `feed` from Serial → servo + food + beep.
4. Missed-meal alert: `settime` an empty bowl long past fill, wait → "ALERT,MISSED_MEAL".
5. Adaptive learning: mention portion moved after repeated leftovers (check `status` portion vs default 20 g).

## Firmware notes (things that bite)

- **AVR string literals live in RAM.** Every `Serial.println("…")` literal used to
  be copied into the data region, which pushed the sketch to ~81% RAM with a
  "Low memory available" warning. **Bug sweep 2026-10-06 wrapped all Serial
  literals in `F()`** (flash instead of RAM): RAM went **1667 → 1059 bytes
  (81% → 51%)**, stack headroom 381 → 989 bytes. Flash: **72%**. Keep any new
  serial strings behind `F()` too. (`showMessage()` still takes `const char*`
  RAM strings — only 4 one-liners, fine.)
- The headless build (`USE_LCD 0`) does not instantiate the `LiquidCrystal_I2C`
  object at all — it is inside `#if USE_LCD`, saving ~24 bytes plus its vtable.
- **EEPROM calibration flag:** `EE_CAL_SET` uses `CAL_SET_VALUE` (0xC7) to mean
  "calibrated"; plain 0xFF means "never calibrated". Previously both wrote 0xFF,
  so a wiped EEPROM silently reused `-470` as if it were a real calibration.
  `status` now reports `calSet=yes|no`.
- **Servo end stops:** stock 544–2400 µs stalls many servos against their stops,
  which overheats and strips the horn. `smin`/`smax`/`ssave` persist the real
  range to EEPROM so demo day needs no recalibration.

## Bug sweep 2026-10-06 (find-all-and-fix pass; compiles 72% / 51%)

- **Fixed — shared ring index (was the "known open defect"):** the portion ring
  (`leftoverG[]`) and the schedule ring (`minutesToLow[]`) advanced the same
  `adapt[s].w`, so they drifted out of phase and overwrote each other. Now
  `wLeft` / `wTime` are independent write pointers.
- **Fixed — learning ran on an uncalibrated scale:** before `cal`, every reading
  is forced to 0 g. `learnPortion()` saw "0 left over" → portion marched to
  MAX_PORTION (80 g); `checkLowCross()` fired instantly → schedule ring filled
  with ~0 minutes. All four learn/alert paths now bail when `!calReady`.
- **Fixed — uint16 underflow hid the missed-meal alert:** `(uint16_t)fillWeight - 3`
  wrapped to ~65533 when the bowl started near-empty, so `checkMissedMeal()`
  could never fire. Now a signed `lastGrams + 3 >= fillWeight` comparison.
- **Fixed — uncalibrated raw display wrapped:** HX711 raw counts routinely exceed
  65535; `calRawMag` was uint16 and wrapped to garbage on the LCD. Now uint32.
- **Fixed — `reset` kept serving stale calibration:** `factoryReset()` only wiped
  EEPROM; `calReady`/`calValue` (and fill/marker state) stayed in RAM until the
  next reboot. It now resets RAM too and re-points the live scale.
- **Fixed — every motion command was a silent no-op after boot:** the flap is
  deliberately detached at boot, and `Servo.write*` on a detached channel only
  updates RAM. `pulse`, `creep`, `spark`, `ssweep` now attach first
  (`servoEnsureAttached()`).
- **Fixed — portions systematically under-dispensed:** `grams / GRAMS_PER_CYCLE`
  truncated, so 25 g always fell as 20 g. Now rounds to the nearest cycle.
- **Fixed — `settime` after a same-day feed blocked the re-demo:** the
  "already fed today" day markers weren't cleared, so rewinding the clock then
  firing the meal failed. `settime` clears them.
- **Fixed — "BOWL FULL" skipped meals never taught the learner:** the clearest
  over-feeding signal (bowl ≥80% full at mealtime) now feeds `learnPortion()`.
- **Fixed — 49.7-day rollover:** `millis() < msgExpire` for a delayed message; now
  a signed difference so the boot message can't reappear after 49.7 days.
- **Fixed — learning was RAM-only:** the ring history reset on every power cycle
  (power-bank demo unplugs power to prove the CR2032). Rings now persist to
  EEPROM slots 87..130 with a magic byte + validation, and auto-reset if corrupt.
  EEPROM write/update also switched to `EEPROM.update` (no erase on same-value).
- **Fixed — `status` never printed the meal log** though the README/demo script
  promise it. It now dumps the 10-entry log (`LOG A 08:00 F p=20 left=12`);
  `LOG empty` when blank. Also removed a dead global `line2[]` buffer.
- **Left as-is (by design):** `checkSchedule()` matches the meal minute exactly —
  a missed minute skips that meal, but switching to `>=` would double-feed after
  a mid-day reboot, which is worse for a feeder.
