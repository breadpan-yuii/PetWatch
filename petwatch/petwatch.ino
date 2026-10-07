/*
 * PetWatch: Automated Pet Feeder with Adaptive Schedules - Arduino UNO
 *
 * Libraries (Sketch > Include Library > Manage Libraries...):
 *   - LiquidCrystal_I2C  (Frank de Brabander)  -> search "LiquidCrystal I2C"
 *   - RTClib              (Adafruit)           -> search "RTClib"
 *   - HX711               (Bogdan Necula)      -> search "HX711"
 *
 * Wiring:
 *   Load cell HX711 : VCC 5V  GND GND  DOUT D3  SCK D2
 *   RTC DS3231      : VCC 5V  GND GND  SDA A4  SCL A5
 *   LCD 16x2 I2C    : VCC 5V  GND GND  SDA A4  SCL A5
 *   Servo           : Sig D9  PWR 5V   GND GND
 *   Buzzer          : + D8   - GND
 *
 * Serial commands (Serial Monitor @ 9600 baud):
 *   tare                  -> tare load cell with empty bowl
 *   cal <grams>           -> place known weight, then e.g. "cal 100"
 *   scal                  -> print cal/raw/grams without recalibrating
 *   settime y m d hh mm ss-> set RTC, e.g. "settime 2026 9 18 8 0 0"
 *   feed                  -> force a feeding cycle now
 *   man                   -> manual dispense (button, skips learning)
 *   portion <g>           -> set grams per feeding (5..80)
 *   reset                 -> wipe saved settings
 *   status                -> print settings, calibration, servo range, meal log
 *
 * Servo end-stop calibration (stock 544..2400us stalls many servos against
 * their stops until they overheat and strip the horn):
 *   srange                -> current pulse range, park angle, live width
 *   ssweep                -> slow sweep; any other command stops it
 *   pulse <us> / creep <d>-> drive one width / nudge by a delta
 *   smin <us> / smax <us> -> set calibrated end, re-attach
 *   spark <deg>           -> boot park angle
 *   ssave                 -> persist range + park to EEPROM
 *   soff / son            -> detach (no holding torque) / re-attach
 *
 * NOTE: on AVR every string literal is copied into RAM, so the serial messages
 * are deliberately terse. This sketch sits at ~78% of the UNO's 2KB.
 *
 * Headless mode: set USE_LCD (top of file) to 0 to run without the LCD.
 * The two display lines then stream to the Serial Monitor as "LCD:...".
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <Servo.h>
#include <HX711.h>
#include <EEPROM.h>

// ---------------- USER-TUNABLE SETTINGS ----------------

const uint8_t  DOUT_PIN      = 3;
const uint8_t  SCK_PIN       = 2;
const uint8_t  SERVO_PIN     = 9;
const uint8_t  BUZZER_PIN    = 8;

const uint8_t  LCD_ADDR      = 0x27;   // try 0x3F if blank
const uint8_t  LCD_COLS      = 16;
const uint8_t  LCD_ROWS      = 2;

#define USE_LCD 1                     // 1 = I2C LCD installed; 0 = headless.
                                      // When 0 the LCD lines print to Serial Monitor instead.

const uint8_t  DEFAULT_PORTION = 20;   // grams per feeding
const uint8_t  MIN_PORTION     = 5;
const uint8_t  MAX_PORTION     = 80;
const uint8_t  GRAMS_PER_CYCLE = 10;   // grams dropped per flap cycle (tune)

const uint8_t  MEAL_A_HOUR    = 8;   const uint8_t  MEAL_A_MIN  = 0;
const uint8_t  MEAL_B_HOUR    = 18;  const uint8_t  MEAL_B_MIN  = 0;

const uint8_t  ADAPT_WINDOW   = 5;    // feedings used for learning
const uint8_t  PORTION_STEP   = 15;   // % the portion changes by
const uint16_t SCHEDULE_STEP  = 15;   // minutes a meal shifts earlier
const uint16_t SHIFT_MARGIN   = 45;   // empty this long before next meal -> shift
const uint16_t NOT_EATING_MIN = 180;  // alert after this many minutes untouched

const uint8_t  SERVO_CLOSED   = 15;
const uint8_t  SERVO_OPEN     = 95;
const uint16_t CYCLE_MS       = 700;

// Servo pulse calibration (build-log Stage 3). Stock 544..2400 us drives some
// servos into their end stops, where they stall, heat up and strip the horn.
// Creep the real stops out with `pulse`/`creep`, then `ssave` them to EEPROM.
const uint16_t SERVO_US_DEF_MIN  = 544;
const uint16_t SERVO_US_DEF_MAX  = 2400;
const uint16_t SERVO_US_HARD_MIN = 400;   // never commanded outside this, whatever is stored
const uint16_t SERVO_US_HARD_MAX = 2600;
const uint8_t  SERVO_PARK_DEF    = SERVO_CLOSED;
const uint32_t SERVO_SWEEP_MS    = 15;     // step interval for `sweep`
const int16_t  SERVO_SWEEP_STEP  = 10;     // us per sweep step

const float    DEFAULT_CAL    = -470.0f;

// ---------------- EEPROM MAP ----------------

const uint16_t EE_MAGIC   = 0;
const uint16_t EE_PORTION = 1;
const uint16_t EE_AV_AH   = 2;   const uint16_t EE_AV_AM = 3;
const uint16_t EE_BV_BH   = 4;   const uint16_t EE_BV_BM = 5;
const uint16_t EE_HEAD    = 6;
const uint16_t EE_CAL_SET = 8;
const uint16_t EE_CAL     = 12;  // float, 4 bytes
const uint16_t EE_LOG     = 20;  // 10 x 6 bytes = 20..79
const uint16_t EE_SMIN    = 80;  // servo min pulse us, 2 bytes (80..81)
const uint16_t EE_SMAX    = 82;  // servo max pulse us, 2 bytes (82..83)
const uint16_t EE_SPARK   = 84;  // boot park angle, 1 byte
const uint16_t EE_SSERVO  = 85;  // SERVO_MAGIC: marks the above as user-saved
const uint16_t EE_ADAPT_MAGIC = 86;  // 0xAD = the learning rings below are valid
const uint16_t EE_ADAPT_A     = 87;  // 22 B per slot: 5x int16 + 5x uint16 + 2 indices
                                     // slot A 87..108, slot B 109..130
const uint16_t EE_ADAPT_BYTES = 22;
const uint8_t  ADAPT_MAGIC    = 0xAD;

const uint8_t  MAGIC_VALUE   = 0x5A;
const uint8_t  CAL_SET_VALUE = 0xC7;   // distinct from EEPROM's erased 0xFF,
                                      // so "never calibrated" is detectable
const uint8_t  SERVO_MAGIC   = 0xA5;   // marks the servo range as user-saved
const uint8_t  LOG_SIZE      = 10;
const uint8_t  LOG_ENTRY_BYT = 6;

const uint8_t  OUT_FILLED  = 0;
const uint8_t  OUT_SKIPPED = 1;
const uint8_t  OUT_MISSED  = 2;

const uint8_t  SLOT_A = 0;
const uint8_t  SLOT_B = 1;
const uint8_t  NUM_SLOTS = 2;

// ---------------- GLOBALS ----------------

RTC_DS3231        rtc;
#if USE_LCD
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);   // 24B RAM + vtable: headless builds skip it
#endif
HX711             scale;
Servo             flap;

// Calibrated servo pulse widths. SERVO_OPEN/SERVO_CLOSED stay in degrees so the
// feeding code is unchanged; the degrees are mapped onto whatever physical range
// `ssave` recorded for the fitted servo.
uint16_t servoMinUs  = SERVO_US_DEF_MIN;
uint16_t servoMaxUs  = SERVO_US_DEF_MAX;
uint8_t  servoPark   = SERVO_PARK_DEF;
uint16_t servoLiveUs  = SERVO_US_DEF_MIN;
bool     servoSweeping = false;
uint8_t  servoSweepDir = 1;
uint32_t servoSweepMs = 0;

struct Settings {
  uint8_t portion;
  uint8_t hourA, minA;
  uint8_t hourB, minB;
} cfg;

float   calValue = DEFAULT_CAL;
bool    scaleReady = false;
uint8_t lastDayA = 0, lastDayB = 0;
uint8_t logHead  = 0;

struct Adapt {
  int16_t  leftoverG[ADAPT_WINDOW];    // bowl grams when the next meal was due
  uint16_t minutesToLow[ADAPT_WINDOW]; // feed -> bowl went low; 0xFFFF = no data
  uint8_t  wLeft;                      // the two rings fill at different times
  uint8_t  wTime;                      // (portion on feed, schedule on empty),
};                                      // so they must not share a write index
Adapt adapt[NUM_SLOTS];

bool     filledOnce   = false;
uint8_t  fillSlot     = 0;
uint16_t fillStartMin = 0;
int16_t  fillWeight   = 0;
bool     lowCrossed   = true;

char     line1[17], msgLine[17];     // no global line2: updateDisplay() builds its
                                     // own pointer, the array was dead 17 bytes
bool     rtcOk = false;
uint32_t lastRtcRetry = 0;
uint32_t msgExpire  = 0;
uint32_t lastLcd    = 0;
uint32_t lastMirror = 0;
uint32_t lastWeight = 0;
int16_t  lastGrams  = 0;

// ---------------- HELPERS ----------------

uint16_t minuteOfDay(uint8_t h, uint8_t m) { return (uint16_t)h * 60 + m; }

// True only after `cal` has stored a real factor. Until then DEFAULT_CAL is a
// guess, and reporting the resulting garbage as "0 g" is worse than useless:
// it reads as "empty bowl" and hides a perfectly healthy load cell. Report the
// raw magnitude on the LCD instead so the signal stays visible.
bool     calReady     = false;
uint32_t calRawMag    = 0;   // uint32: HX711 raw counts routinely exceed 65535 and
                             // a uint16 truncation wrapped them to garbage on-screen

// The HX711's read() spins on `while (DOUT != LOW)` with no timeout, so a load
// cell that dies or is unplugged mid-run freezes the whole loop: no display, no
// serial, no feeding. Every block of scale reads must be gated on a bounded
// readiness wait, and a lost cell drops the scale instead of bricking the feeder.
bool scaleAliveNow() {
  if (scale.wait_ready_timeout(120, 10)) return true;
  scaleReady = false;
  Serial.println(F("WARN,scale_lost"));
  return false;
}

int16_t readGrams() {
  if (!scaleReady) return 0;
  if (!scaleAliveNow()) return 0;
  if (!calReady) {
    long r = scale.get_value(1);
    int32_t mag = (r < 0) ? -r : r;
    calRawMag = constrain(mag, 0, 99999);
    return 0;                       // no trustworthy unit conversion yet
  }
  float g = scale.get_units(3);
  if (g < 0) g = 0;
  return (int16_t)(g + 0.5f);
}

void showMessage(const char* m, uint16_t ms) {
  strncpy(msgLine, m, 16);
  msgLine[16] = '\0';
  msgExpire = millis() + ms;
}

void beep(uint8_t times, uint16_t onMs, uint16_t gapMs) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    delay(gapMs);
  }
}

// ---------------- SERVO RANGE CALIBRATION ----------------

// A stock servo rarely reaches 0/180 cleanly; at 544/2400 us many stall against
// their end stops and overheat. These map the logical 0..180 used by
// SERVO_OPEN/SERVO_CLOSED onto the pulse widths the *fitted* servo actually
// tracks, and are persisted with `ssave` so the demo doesn't need recalibrating.

void servoAttach() {
  flap.attach(SERVO_PIN, servoMinUs, servoMaxUs);
}

// The flap is deliberately detached at boot (holding a stalled servo against its
// stop brownout-resets the UNO). Servo.write* on a detached channel updates RAM
// only — no pulses — so every explicit motion command must attach first, or
// `pulse 1500` looks like it works and nothing moves.
void servoEnsureAttached() {
  if (!flap.attached()) servoAttach();
}

void servoWriteAngle(uint8_t deg) {
  servoLiveUs = map(deg, 0, 180, servoMinUs, servoMaxUs);
  flap.write(deg);
}

void servoWriteUs(uint16_t us) {
  servoLiveUs = constrain(us, (uint16_t)SERVO_US_HARD_MIN, (uint16_t)SERVO_US_HARD_MAX);
  flap.writeMicroseconds(servoLiveUs);
}

void servoLoadRange() {
  if (EEPROM.read(EE_SSERVO) != SERVO_MAGIC) {
    Serial.print(F("SERVO,default"));
    Serial.print(servoMinUs);
    Serial.print(F(".."));
    Serial.println(servoMaxUs);
    return;
  }
  uint16_t mn = (uint16_t)EEPROM.read(EE_SMIN) | ((uint16_t)EEPROM.read(EE_SMIN + 1) << 8);
  uint16_t mx = (uint16_t)EEPROM.read(EE_SMAX) | ((uint16_t)EEPROM.read(EE_SMAX + 1) << 8);
  mn = constrain(mn, (uint16_t)SERVO_US_HARD_MIN, (uint16_t)SERVO_US_HARD_MAX);
  mx = constrain(mx, (uint16_t)SERVO_US_HARD_MIN, (uint16_t)SERVO_US_HARD_MAX);
  if (mn >= mx) {
    Serial.println(F("SERVO,bad_range"));
    servoMinUs = SERVO_US_DEF_MIN;
    servoMaxUs = SERVO_US_DEF_MAX;
    return;
  }
  servoMinUs = mn;
  servoMaxUs = mx;
  servoPark  = constrain(EEPROM.read(EE_SPARK), 0, 180);
  Serial.print(F("SERVO,range="));
  Serial.print(servoMinUs);
  Serial.print(F(".."));
  Serial.print(servoMaxUs);
  Serial.println();
}

void servoSaveRange() {
  EEPROM.update(EE_SMIN,   servoMinUs & 0xFF);
  EEPROM.update(EE_SMIN+1, servoMinUs >> 8);
  EEPROM.update(EE_SMAX,   servoMaxUs & 0xFF);
  EEPROM.update(EE_SMAX+1, servoMaxUs >> 8);
  EEPROM.update(EE_SPARK,  servoPark);
  EEPROM.update(EE_SSERVO, SERVO_MAGIC);
  Serial.println(F("SERVO,saved"));
}

// Slow sweep between the calibrated ends, used to find where the horn actually
// stops. Driven from loop() so Serial stays responsive and any command cancels it.
void servoStartSweep() {
  servoEnsureAttached();
  servoSweeping = true;
  servoSweepDir = 1;
  servoSweepMs  = millis();
  Serial.println(F("SERVO,sweeping"));
}

void servoStopSweep() {
  servoSweeping = false;
}

void servoSweepTick() {
  if (!servoSweeping) return;
  if ((int32_t)(millis() - servoSweepMs) < (int32_t)SERVO_SWEEP_MS) return;
  servoSweepMs = millis();
  int32_t next = (int32_t)servoLiveUs + (int32_t)SERVO_SWEEP_STEP * servoSweepDir;
  if (next >= (int32_t)servoMaxUs) { next = servoMaxUs; servoSweepDir = -1; }
  if (next <= (int32_t)servoMinUs) { next = servoMinUs; servoSweepDir = 1; }
  servoWriteUs((uint16_t)next);
}

void adaptReset(uint8_t s) {
  for (uint8_t i = 0; i < ADAPT_WINDOW; i++) {
    adapt[s].leftoverG[i]    = -1;
    adapt[s].minutesToLow[i] = 0xFFFF;
  }
  adapt[s].wLeft = 0;
  adapt[s].wTime = 0;
}

// The learning windows only ever hold 5 meals, so with feeds twice a day an
// EEPROM write costs essentially nothing — but losing them on every power cycle
// (battery swap, demo unplugging) silently reset the adaptive behaviour.
void adaptSave(uint8_t s) {
  uint16_t base = EE_ADAPT_A + (uint16_t)s * EE_ADAPT_BYTES;
  for (uint8_t i = 0; i < ADAPT_WINDOW; i++) {
    EEPROM.put(base + i * 2,      adapt[s].leftoverG[i]);
    EEPROM.put(base + 10 + i * 2, adapt[s].minutesToLow[i]);
  }
  EEPROM.update(base + 20, adapt[s].wLeft);
  EEPROM.update(base + 21, adapt[s].wTime);
  EEPROM.update(EE_ADAPT_MAGIC, ADAPT_MAGIC);
}

// Reload the rings, validating every field: an erased or corrupted block must
// fall back to an empty window rather than feed junk into the learner.
void adaptLoadBroken() {
  for (uint8_t s = 0; s < NUM_SLOTS; s++) adaptReset(s);
  EEPROM.update(EE_ADAPT_MAGIC, 0xFF);
  Serial.println(F("WARN,learn_data_bad_reset"));
}

void adaptLoad() {
  if (EEPROM.read(EE_ADAPT_MAGIC) != ADAPT_MAGIC) return;
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    uint16_t base = EE_ADAPT_A + (uint16_t)s * EE_ADAPT_BYTES;
    uint8_t wl = EEPROM.read(base + 20);
    uint8_t wt = EEPROM.read(base + 21);
    if (wl >= ADAPT_WINDOW || wt >= ADAPT_WINDOW) { adaptLoadBroken(); return; }
    adapt[s].wLeft = wl;
    adapt[s].wTime = wt;
    for (uint8_t i = 0; i < ADAPT_WINDOW; i++) {
      int16_t  lg;
      uint16_t mt;
      EEPROM.get(base + i * 2,      lg);
      EEPROM.get(base + 10 + i * 2, mt);
      if (lg < -1 || lg > 1000 || (mt != 0xFFFF && mt > 24 * 60)) {
        adaptLoadBroken();
        return;
      }
      adapt[s].leftoverG[i]    = lg;
      adapt[s].minutesToLow[i] = mt;
    }
  }
}

// ---------------- EEPROM PERSISTENCE ----------------

void saveConfig() {
  EEPROM.update(EE_PORTION, cfg.portion);
  EEPROM.update(EE_AV_AH, cfg.hourA);
  EEPROM.update(EE_AV_AM, cfg.minA);
  EEPROM.update(EE_BV_BH, cfg.hourB);
  EEPROM.update(EE_BV_BM, cfg.minB);
  EEPROM.update(EE_HEAD, logHead);
}

void factoryReset() {
  cfg.portion = DEFAULT_PORTION;
  cfg.hourA = MEAL_A_HOUR; cfg.minA = MEAL_A_MIN;
  cfg.hourB = MEAL_B_HOUR; cfg.minB = MEAL_B_MIN;
  logHead = 0;
  // RAM state must follow the EEPROM state, otherwise `reset` keeps serving the
  // old calibration (and stale meal/learning state) until the next reboot.
  calValue  = DEFAULT_CAL;
  calReady  = false;
  filledOnce = false;
  lowCrossed = true;
  fillWeight = 0;
  fillStartMin = 0;
  lastDayA = 0;
  lastDayB = 0;
  EEPROM.write(EE_MAGIC, MAGIC_VALUE);
  EEPROM.write(EE_CAL_SET, 0xFF);   // 0xFF = "never calibrated", unlike CAL_SET_VALUE
  EEPROM.put(EE_CAL, DEFAULT_CAL);
  EEPROM.update(EE_ADAPT_MAGIC, 0xFF);
  saveConfig();
  for (uint8_t s = 0; s < NUM_SLOTS; s++) adaptReset(s);
  if (scaleReady) {                 // setup() calls this before scale.begin()
    scale.set_scale(calValue);
    scale.tare();
  }
}

void loadConfig() {
  float c = DEFAULT_CAL;
  if (EEPROM.read(EE_MAGIC) != MAGIC_VALUE) {
    factoryReset();
  } else {
    cfg.portion = EEPROM.read(EE_PORTION);
    cfg.hourA = EEPROM.read(EE_AV_AH); cfg.minA = EEPROM.read(EE_AV_AM);
    cfg.hourB = EEPROM.read(EE_BV_BH); cfg.minB = EEPROM.read(EE_BV_BM);
    logHead = EEPROM.read(EE_HEAD) % LOG_SIZE;
  }
  calReady = false;
  if (EEPROM.read(EE_CAL_SET) == CAL_SET_VALUE) {
    EEPROM.get(EE_CAL, c);
    if (c != c || c == 0.0f) {
      calValue = DEFAULT_CAL;
      Serial.println(F("WARN,cal_corrupt_using_default"));
    } else {
      calValue = c;
      calReady = true;
    }
  } else {
    calValue = DEFAULT_CAL;
    Serial.println(F("WARN,not_calibrated_run_cal"));
  }
  for (uint8_t s = 0; s < NUM_SLOTS; s++) adaptReset(s);
  adaptLoad();                       // keeps the learning windows across power-off
}

void logEntry(uint8_t slot, uint8_t outcome, int16_t leftover) {
  if (!rtcOk) return;
  uint16_t a = EE_LOG + (uint16_t)logHead * LOG_ENTRY_BYT;
  DateTime now = rtc.now();
  EEPROM.update(a + 0, cfg.portion);
  EEPROM.update(a + 1, slot);
  EEPROM.update(a + 2, now.hour());
  EEPROM.update(a + 3, now.minute());
  EEPROM.update(a + 4, (uint8_t)constrain(leftover, -128, 127));
  EEPROM.update(a + 5, outcome);
  logHead = (logHead + 1) % LOG_SIZE;
  EEPROM.update(EE_HEAD, logHead);
}

// ---------------- ADAPTIVE (LEARNING) LOGIC ----------------

int16_t avgLeftover(uint8_t s) {
  long sum = 0; uint8_t n = 0;
  for (uint8_t i = 0; i < ADAPT_WINDOW; i++) {
    if (adapt[s].leftoverG[i] >= 0) { sum += adapt[s].leftoverG[i]; n++; }
  }
  return (n == 0) ? -1 : (int16_t)(sum / n);
}

void learnPortion(uint8_t s, int16_t leftover) {
  // Before `cal` every reading is forced to 0 g, and an average of 0 reads as
  // "bowl was licked clean" -> the portion would march straight to MAX_PORTION.
  if (!calReady) return;
  adapt[s].leftoverG[adapt[s].wLeft] = leftover;
  adapt[s].wLeft = (adapt[s].wLeft + 1) % ADAPT_WINDOW;
  adaptSave(s);
  int16_t avg = avgLeftover(s);
  if (avg < 0) return;
  uint8_t old = cfg.portion;
  if (avg > cfg.portion * 30 / 100) {            // too much left each meal
    cfg.portion = max(MIN_PORTION,
      (uint8_t)(cfg.portion * (100 - PORTION_STEP) / 100));
  } else if (avg <= cfg.portion * 5 / 100) {     // bowl basically empty
    cfg.portion = min(MAX_PORTION,
      (uint8_t)(cfg.portion * (100 + PORTION_STEP) / 100));
  }
  if (cfg.portion != old) {
    saveConfig();
    char buf[17];
    snprintf(buf, sizeof(buf), "PORTION -> %ug", cfg.portion);
    showMessage(buf, 4000);
    beep(1, 120, 60);
    Serial.println(buf);
  }
}

void learnSchedule(uint8_t s, uint16_t minsToLow) {
  if (!calReady) return;            // see learnPortion(): 0 g readings are junk
  if (minsToLow == 0xFFFF) return;
  adapt[s].minutesToLow[adapt[s].wTime] = minsToLow;
  adapt[s].wTime = (adapt[s].wTime + 1) % ADAPT_WINDOW;
  adaptSave(s);                     // save here: the shift below may not happen

  long sum = 0; uint8_t n = 0;
  for (uint8_t i = 0; i < ADAPT_WINDOW; i++) {
    if (adapt[s].minutesToLow[i] != 0xFFFF) { sum += adapt[s].minutesToLow[i]; n++; }
  }
  if (n < ADAPT_WINDOW) return;

  uint16_t avg = (uint16_t)(sum / n);
  uint16_t currMin = (s == SLOT_A)
    ? minuteOfDay(cfg.hourA, cfg.minA)
    : minuteOfDay(cfg.hourB, cfg.minB);
  uint16_t nextMin = (s == SLOT_A)
    ? minuteOfDay(cfg.hourB, cfg.minB)
    : minuteOfDay(cfg.hourA, cfg.minA) + 24 * 60;
  uint16_t gap = nextMin - currMin;
  uint16_t lowClamp  = (s == SLOT_A) ? 6 * 60 : 17 * 60;
  uint16_t highClamp = (s == SLOT_A) ? (nextMin - 3 * 60) : 22 * 60;

  if (avg + SHIFT_MARGIN <= gap && currMin + SCHEDULE_STEP >= lowClamp) {
    uint16_t nMin = currMin - SCHEDULE_STEP;
    if (nMin < lowClamp) nMin = lowClamp;
    if (nMin > highClamp) return;
    if (s == SLOT_A) { cfg.hourA = nMin / 60; cfg.minA = nMin % 60; }
    else             { cfg.hourB = nMin / 60; cfg.minB = nMin % 60; }
    saveConfig();
    char buf[17];
    snprintf(buf, sizeof(buf), "MEAL -> %02u:%02u",
             (s == SLOT_A) ? cfg.hourA : cfg.hourB,
             (s == SLOT_A) ? cfg.minA : cfg.minB);
    showMessage(buf, 4000);
    beep(2, 120, 80);
    Serial.println(buf);
  }
}

// ---------------- FEEDING ----------------

void dispense(uint8_t grams) {
  // Round to the nearest cycle: plain division silently rounded every portion
  // down (25 g -> 2 cycles -> 20 g), so the pet was systematically under-fed.
  uint8_t cycles = (uint8_t)((grams + GRAMS_PER_CYCLE / 2) / GRAMS_PER_CYCLE);
  if (cycles < 1) cycles = 1;
  servoAttach();                    // only draw current when actually dispensing
  flap.write(SERVO_CLOSED);
  delay(CYCLE_MS);
  for (uint8_t c = 0; c < cycles; c++) {
    flap.write(SERVO_OPEN);
    delay(CYCLE_MS);
    flap.write(SERVO_CLOSED);
    delay(CYCLE_MS);
  }
  flap.detach();                    // release holding torque afterwards
}

// Manual top-up button (D4 -> GND, no resistor needed: internal pull-up). It
// dispenses one portion but deliberately does NOT touch filledOnce/fillStartMin
// or the learning rings, so a human snack can't skew the adaptive schedule or
// shrink the auto portion.
const uint8_t MANUAL_PIN = 4;
bool btnPrev = HIGH;
uint16_t btnDebounceTs = 0;

void manualDispense() {
  uint8_t p = cfg.portion;
  dispense(p);
  beep(1, 120, 0);
  char buf[17];
  snprintf(buf, sizeof(buf), "MANUAL +%ug", p);
  showMessage(buf, 3000);
  Serial.print(F("FEED,MANUAL,portion="));
  Serial.println(p);
}

void handleManualButton() {
  uint8_t raw = digitalRead(MANUAL_PIN);
  if (btnDebounceTs && (uint16_t)(millis() - btnDebounceTs) < 40) return;
  btnDebounceTs = 0;
  if (raw == btnPrev) return;
  btnDebounceTs = millis();
  btnPrev = raw;
  if (raw == LOW) manualDispense();
}

void processFeeding(uint8_t slot) {
  DateTime now = rtc.now();
  int16_t leftover = readGrams();
  uint8_t portion = cfg.portion;

  if (leftover >= portion * 8 / 10) {
    logEntry(slot, OUT_SKIPPED, leftover);
    learnPortion(slot, leftover);    // a bowl still 80% full is the strongest
    char buf[17];                    // "you are feeding too much" signal there is
    snprintf(buf, sizeof(buf), "BOWL FULL %dg", leftover);
    showMessage(buf, 3000);
    return;
  }

  dispense(portion);
  delay(2500);                       // let food settle
  int16_t after = readGrams();

  filledOnce = true;
  lowCrossed = false;
  fillSlot   = slot;
  fillStartMin = (uint16_t)now.hour() * 60 + now.minute();
  fillWeight   = after;

  logEntry(slot, OUT_FILLED, leftover);

  char buf[17];
  snprintf(buf, sizeof(buf), "FED %ug +%dg", portion, leftover);
  showMessage(buf, 3000);
  beep(2, 100, 60);
  Serial.print(F("FEED,FILLED,slot="));
  Serial.print(slot);
  Serial.print(F(",portion="));
  Serial.print(portion);
  Serial.print(F(",leftover="));
  Serial.print(leftover);
  Serial.print(F(",bowlAfter="));
  Serial.println(after);

  learnPortion(slot, leftover);
}

void checkLowCross() {
  if (!rtcOk) return;
  if (!calReady) return;             // uncalibrated grams are always 0 -> instant
  if (!filledOnce) return;           // "the pet ate everything" on the first loop
  if (lowCrossed) return;
  int16_t w = lastGrams;
  uint16_t threshold = (uint16_t)(cfg.portion * 25 / 100);
  if (w >= 0 && (uint16_t)w <= threshold) {
    lowCrossed = true;
    uint16_t nowMin = (uint16_t)rtc.now().hour() * 60 + rtc.now().minute();
    uint16_t elapsed = (nowMin >= fillStartMin) ? nowMin - fillStartMin
                                                : nowMin + 24 * 60 - fillStartMin;
    learnSchedule(fillSlot, elapsed);
    Serial.print(F("EATEN,slot="));
    Serial.print(fillSlot);
    Serial.print(F(",minutesToLow="));
    Serial.println(elapsed);
  }
}

void checkMissedMeal() {
  if (!rtcOk) return;
  if (!calReady) return;
  if (!filledOnce) return;
  if (lowCrossed) return;
  uint16_t nowMin = (uint16_t)rtc.now().hour() * 60 + rtc.now().minute();
  uint16_t elapsed = (nowMin >= fillStartMin) ? nowMin - fillStartMin
                                              : nowMin + 24 * 60 - fillStartMin;
  if (elapsed < NOT_EATING_MIN) return;
  // "unchanged since the feed" = within 3 g of the weight recorded then. Done in
  // signed math: the old (uint16_t)fillWeight - 3 underflowed to ~65535 whenever
  // the bowl started near empty, so the alert could never fire.
  if (lastGrams + 3 >= fillWeight) {
    lowCrossed = true;
    logEntry(fillSlot, OUT_MISSED, lastGrams);
    showMessage("NOT EATING!", 5000);
    beep(3, 200, 150);
    Serial.println(F("ALERT,MISSED_MEAL"));
  }
}

// ---------------- SCHEDULE CHECK ----------------

void checkSchedule() {
  if (!rtcOk) return;
  DateTime now = rtc.now();
  uint16_t mm = (uint16_t)now.hour() * 60 + now.minute();
  if (mm == minuteOfDay(cfg.hourA, cfg.minA) && now.day() != lastDayA) {
    lastDayA = now.day();
    processFeeding(SLOT_A);
  }
  if (mm == minuteOfDay(cfg.hourB, cfg.minB) && now.day() != lastDayB) {
    lastDayB = now.day();
    processFeeding(SLOT_B);
  }
}

// ---------------- SERIAL COMMANDS ----------------

void runCommand(char* s);

char cmd[40];
uint8_t cmdLen = 0;

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      cmd[cmdLen] = '\0';
      runCommand(cmd);
      cmdLen = 0;
    } else if (cmdLen < sizeof(cmd) - 1) {
      cmd[cmdLen++] = c;
    }
  }
}

void runCommand(char* s) {
  if (s[0] == '\0') return;
  servoStopSweep();                 // any command cancels a running sweep

  if (strncmp(s, "tare", 4) == 0) {
    if (!scaleReady || !scaleAliveNow()) { Serial.println(F("WARN,no_scale")); }
    else {
      scale.tare();
      showMessage("TARED 0g", 2000);
      Serial.println(F("OK,tared"));
    }
  } else if (strncmp(s, "cal ", 4) == 0) {
    if (!scaleReady || !scaleAliveNow()) { Serial.println(F("WARN,no_scale")); }
    else {
      int known = atoi(s + 4);
      if (known > 0) {
        // 4 samples, not 10: at the HX711's power-on rate of 10Hz, 10 samples
        // takes a full second and people assume the board ignored them.
        float raw = scale.get_value(4);
        int32_t mag = (raw < 0) ? -raw : raw;
        if (mag < 100) {
          Serial.println(F("ERR,raw_small"));
        } else {
          calValue = raw / (float)known;
          scale.set_scale(calValue);
          calReady = true;
          EEPROM.write(EE_CAL_SET, CAL_SET_VALUE);
          EEPROM.put(EE_CAL, calValue);
          Serial.print(F("OK,cal="));
          Serial.print(calValue, 6);
          Serial.print(F("  cp/g="));
          Serial.println(raw / (float)known);
          showMessage("CAL OK", 2000);
        }
      } else {
        Serial.println(F("usage: cal <grams>"));
      }
    }
  } else if (strcmp(s, "scal") == 0) {
    if (!scaleReady || !scaleAliveNow()) { Serial.println(F("WARN,no_scale")); }
    else {
      Serial.print(F("calSet="));
      Serial.print(calReady ? "yes" : "no");
      Serial.print(F("  cal="));
      Serial.print(calValue, 6);
      Serial.print(F("  raw="));
      Serial.print(scale.get_value(4));
      Serial.print(F("  grams="));
      Serial.println(scale.get_units(3), 1);
    }
  } else if (strncmp(s, "settime", 7) == 0) {
    int y, mo, d, h, mi, se;
    if (sscanf(s + 7, "%d %d %d %d %d %d", &y, &mo, &d, &h, &mi, &se) == 6) {
      rtc.adjust(DateTime(y, mo, d, h, mi, se));
      // Clear the "already fed today" markers: rewinding the clock for a demo
      // must let today's meal fire again, otherwise the guard blocks it.
      lastDayA = 0;
      lastDayB = 0;
      showMessage("TIME SET", 2000);
      Serial.println(F("OK,time set"));
    }
  } else if (strcmp(s, "feed") == 0) {
    processFeeding(SLOT_A);
  } else if (strcmp(s, "man") == 0) {
    manualDispense();
  } else if (strncmp(s, "portion ", 8) == 0) {
    int p = atoi(s + 8);
    if (p < (int)MIN_PORTION || p > (int)MAX_PORTION) {
      Serial.print(F("usage: portion "));
      Serial.print((int)MIN_PORTION);
      Serial.print(F(".."));
      Serial.println((int)MAX_PORTION);
    } else {
      cfg.portion = (uint8_t)p;
      saveConfig();
      Serial.print(F("OK,portion="));
      Serial.println(cfg.portion);
    }
  } else if (strcmp(s, "reset") == 0) {
    factoryReset();                  // also re-points the live scale at DEFAULT_CAL
    showMessage("RESET OK", 2000);
    Serial.println(F("OK,reset"));
  } else if (strcmp(s, "status") == 0) {
    Serial.print(F("portion="));
    Serial.println(cfg.portion);
    Serial.print(F("mealA="));
    Serial.print(cfg.hourA);
    Serial.print(F(":"));
    Serial.println(cfg.minA);
    Serial.print(F("mealB="));
    Serial.print(cfg.hourB);
    Serial.print(F(":"));
    Serial.println(cfg.minB);
    Serial.print(F("cal="));
    Serial.print(calValue, 6);
    Serial.print(F("  calSet="));
    Serial.print(EEPROM.read(EE_CAL_SET) == CAL_SET_VALUE ? "yes" : "no");
    Serial.println();
    Serial.print(F("bowl="));
    Serial.println(lastGrams);
    Serial.print(F("scale="));
    Serial.println(scaleReady ? "ok" : "absent");
    Serial.print(F("servo="));
    Serial.print(servoMinUs);
    Serial.print(F(".."));
    Serial.print(servoMaxUs);
    Serial.print(F("  park="));
    Serial.println(servoPark);
    // Meal log — the README/demo script say `status` shows it after a power
    // cycle, and it is the evidence that EEPROM persistence works.
    bool any = false;
    for (uint8_t i = 0; i < LOG_SIZE; i++) {
      uint16_t a = EE_LOG + (uint16_t)i * LOG_ENTRY_BYT;
      uint8_t out = EEPROM.read(a + 5);
      if (out > OUT_MISSED) continue;               // 0xFF = never written
      any = true;
      Serial.print(F("LOG "));
      Serial.print(EEPROM.read(a + 1) == SLOT_A ? 'A' : 'B');
      Serial.print(' ');
      if (EEPROM.read(a + 2) < 10) Serial.print('0');
      Serial.print(EEPROM.read(a + 2));
      Serial.print(':');
      if (EEPROM.read(a + 3) < 10) Serial.print('0');
      Serial.print(EEPROM.read(a + 3));
      Serial.print(' ');
      Serial.print(out == OUT_FILLED ? 'F' : out == OUT_SKIPPED ? 'S' : 'M');
      Serial.print(F(" p="));
      Serial.print(EEPROM.read(a + 0));
      Serial.print(F(" left="));
      Serial.println((int)(int8_t)EEPROM.read(a + 4));
    }
    if (!any) Serial.println(F("LOG empty"));
  } else if (strcmp(s, "srange") == 0) {
    Serial.print(F("SERVO,range="));
    Serial.print(servoMinUs);
    Serial.print(F(".."));
    Serial.print(servoMaxUs);
    Serial.print(F(",park="));
    Serial.print(servoPark);
    Serial.print(F(",live="));
    Serial.println(servoLiveUs);
  } else if (strncmp(s, "pulse ", 6) == 0) {
    int us = atoi(s + 6);
    if (us > 0) { servoEnsureAttached(); servoWriteUs((uint16_t)us); }
    else        { Serial.println(F("usage: pulse <us>")); }
  } else if (strncmp(s, "creep ", 6) == 0) {
    servoEnsureAttached();
    servoWriteUs((uint16_t)((int32_t)servoLiveUs + atoi(s + 6)));
  } else if (strncmp(s, "smin ", 5) == 0) {
    servoMinUs = constrain(atoi(s + 5), (int)SERVO_US_HARD_MIN, (int)SERVO_US_HARD_MAX);
    if (servoMinUs >= servoMaxUs) { Serial.println(F("ERR,bad_min")); }
    else {
      servoAttach();
      Serial.print(F("SERVO,min="));
      Serial.println(servoMinUs);
    }
  } else if (strncmp(s, "smax ", 5) == 0) {
    servoMaxUs = constrain(atoi(s + 5), (int)SERVO_US_HARD_MIN, (int)SERVO_US_HARD_MAX);
    if (servoMinUs >= servoMaxUs) { Serial.println(F("ERR,bad_max")); }
    else {
      servoAttach();
      Serial.print(F("SERVO,max="));
      Serial.println(servoMaxUs);
    }
  } else if (strncmp(s, "spark ", 6) == 0) {
    servoPark = constrain(atoi(s + 6), 0, 180);
    servoEnsureAttached();
    servoWriteAngle(servoPark);
    Serial.print(F("SERVO,park="));
    Serial.println(servoPark);
  } else if (strcmp(s, "ssave") == 0) {
    servoSaveRange();
  } else if (strcmp(s, "ssweep") == 0) {
    servoStartSweep();
  } else if (strcmp(s, "soff") == 0) {
    flap.detach();
    Serial.println(F("SERVO,detached"));
  } else if (strcmp(s, "son") == 0) {
    servoAttach();
    servoWriteAngle(servoPark);
    Serial.println(F("SERVO,attached"));
  } else if (strcmp(s, "lcdi") == 0) {
    lcdReset();
    Serial.println(F("LCD,reinit"));
  }
}

// ---------------- DISPLAY ----------------

// This DS3231 module wedges SDA low after warm-up (documented in build-log), which
// locks out every I2C slave while it lasts — the LCD keeps backlight but stops
// rendering. A 1 Hz probe of the bus detects a wedge; 9 SCL toggle pulses release
// a slave held mid-byte, and the LCD gets re-initialised so the display comes back
// without a reboot.
uint32_t lastBusProbe = 0;
uint32_t busFailures  = 0;
bool     busStuckReported = false;
uint32_t lastStuckPrint   = 0;

void lcdReset() {
  #if USE_LCD
  lcd.init();
  lcd.backlight();
  #endif
}

void probeBus() {
  Wire.beginTransmission(LCD_ADDR);
  if (Wire.endTransmission() == 0) return;   // bus healthy

  busFailures++;
  pinMode(SDA, INPUT_PULLUP);                // release SDA, never drive it
  pinMode(SCL, INPUT_PULLUP);
  for (uint8_t i = 0; i < 9; i++) {
    pinMode(SCL, OUTPUT); digitalWrite(SCL, LOW);
    delayMicroseconds(5);
    pinMode(SCL, INPUT_PULLUP);
    delayMicroseconds(5);
  }
  Wire.begin();
  Wire.setWireTimeout(50000, true);
  digitalWrite(SDA, 1);
  digitalWrite(SCL, 1);

  Wire.beginTransmission(LCD_ADDR);
  if (Wire.endTransmission() != 0) {
    if (!busStuckReported || (uint32_t)(millis() - lastStuckPrint) >= 30000) {
      lastStuckPrint = millis();
      busStuckReported = true;
      Serial.println(F("WARN,bus_stuck"));
    }
    return;
  }
  if (busStuckReported) {
    busStuckReported = false;
    Serial.println(F("WARN,bus_recovered"));
  }
  lcdReset();
}

// Overlap-free with minimal bus traffic: a line is re-sent to the LCD only when
// its text changes, and a shorter line pads spaces over the tail of whatever was
// there before — never a full 16-column rewrite every refresh. Fewer I2C
// transactions = fewer chances for the wedge-prone DS3231 to corrupt a byte mid-
// write (which is what blanked the panel when the display was fully padded).
char    lineCache[2][17];
bool    lineCacheReady = false;

void lcdRender(uint8_t row, const char* s) {
  char* prev = lineCache[row];
  if (lineCacheReady && strncmp(s, prev, 16) == 0) return;
  uint8_t newLen = strlen(s);
  uint8_t oldLen = strlen(prev);
  lcd.setCursor(0, row);
  lcd.print(s);
  for (uint8_t i = newLen; i < oldLen; i++) lcd.print(' ');
  strncpy(prev, s, 16);
  prev[16] = '\0';
  lineCacheReady = true;
}

void updateDisplay() {
  uint32_t now = millis();
  if (now - lastBusProbe >= 1000) { lastBusProbe = now; probeBus(); }
  if (now - lastLcd < 200) return;
  lastLcd = now;

  char wbuf[8];
  if (calReady || !scaleReady) {
    snprintf(wbuf, sizeof(wbuf), "%3ug", (unsigned)constrain(lastGrams, 0, 999));
  } else {
    // Uncalibrated: "0g" would read as an empty bowl and hide a live cell, so
    // show the raw magnitude instead. Nobody calibrates from a blank screen.
    snprintf(wbuf, sizeof(wbuf), "%4u", (unsigned)constrain(calRawMag, 0, 9999));
  }

  DateTime t;
  if (!rtcOk) {
    snprintf(line1, sizeof(line1), "--:--    %s", wbuf);
  } else {
    t = rtc.now();
    watchClockSample(t);
    uint8_t h12 = t.hour() % 12;
    if (h12 == 0) h12 = 12;
    snprintf(line1, sizeof(line1), "%02u:%02u:%02u %s %s",
             (unsigned)h12, t.minute(), t.second(),
             (t.hour() < 12) ? "AM" : "PM", wbuf);
  }

  char status2[17];
  if (!calReady && scaleReady) {
    strncpy(status2, "NEED `cal`", sizeof(status2));
  } else if (!scaleReady) {
    strncpy(status2, "NO CELL", sizeof(status2));
  } else if (filledOnce && !lowCrossed) {
    strncpy(status2, "EATING...", sizeof(status2));
  } else if (lastGrams == 0) {
    strncpy(status2, "BOWL EMPTY", sizeof(status2));
  } else {
    strncpy(status2, "AWAITING MEAL", sizeof(status2));
  }

  // msgExpire is millis()+ms, so a plain `millis() < msgExpire` breaks at the
  // 49.7-day rollover; a signed difference is rollover-safe. msgExpire==0 = no msg.
  char* line2 = (msgExpire && (int32_t)(millis() - msgExpire) < 0) ? msgLine : status2;

  #if USE_LCD
  lcdRender(0, line1);
  lcdRender(1, line2);
#else
  if (now - lastMirror >= 2000) {
    lastMirror = now;
    Serial.print(F("LCD:"));
    Serial.println(line1);
    Serial.print(F("LCD:"));
    Serial.println(line2);
  }
#endif
}

// ---------------- SETUP & LOOP ----------------

// The DS3231 is electrically marginal (needs warm-up, retries, and internal
// pull-ups), so the boot-time detection can miss. Keep retrying in loop() so a
// transient miss self-heals and the clock comes back without a reboot.
void ensureRTC() {
  if (rtcOk) return;
  if (millis() - lastRtcRetry < 5000) return;
  lastRtcRetry = millis();
  Wire.beginTransmission(0x68);
  Wire.endTransmission();
  for (int attempt = 0; attempt < 3; attempt++) {
    if (rtc.begin()) {
      rtcOk = true;
      Serial.println(F("RTC,recovered"));
      return;
    }
    delay(50);
  }
}

// This DS3231 can also freeze mid-run: rtc.now() keeps returning the same stale
// time while rtcOk stays true, so ensureRTC() never re-runs and the clock hangs
// forever. Two 2 s-apart samples showing the same second mean the clock is stuck —
// drop rtcOk and let ensureRTC() re-initialise it.
DateTime lastNowSample;
uint32_t lastNowSampleAt = 0;

void watchClockSample(DateTime t) {
  uint32_t now = millis();
  if (now - lastNowSampleAt < 2000) return;
  if (lastNowSampleAt &&
      t.second() == lastNowSample.second() &&
      t.minute() == lastNowSample.minute()) {
    rtcOk = false;                      // clock is not ticking
    Serial.println(F("ERR,rtc_frozen"));
    return;
  }
  lastNowSample = t;
  lastNowSampleAt = now;
}

void setup() {
  Serial.begin(9600);
  Wire.begin();
  Wire.setWireTimeout(50000, true);   // 50 ms I2C watchdog: a stuck bus can't freeze boot/loop
  digitalWrite(SDA, 1);               // enable internal pull-ups on the I2C bus (this DS3231 module has no/noisy onboard pull-ups)
  digitalWrite(SCL, 1);
#if USE_LCD
  lcd.init();
  lcd.backlight();
#endif
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MANUAL_PIN, INPUT_PULLUP);
  btnPrev = digitalRead(MANUAL_PIN);   // capture idle state so boot doesn't trigger

  // rtc.begin() re-inits Wire internally; as the *first* I2C transaction after boot
  // that double-init always wedges on this DS3231 module. Warm the bus up first.
  Wire.beginTransmission(0x68);
  Wire.endTransmission();
  rtcOk = false;
  for (int attempt = 0; attempt < 3; attempt++) {
    rtcOk = rtc.begin();
    if (rtcOk) break;
    delay(50);
  }
  if (!rtcOk) {
    showMessage("RTC FAIL!", 6000);
    Serial.println(F("ERR,rtc_not_found"));
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    Serial.println(F("WARN,rtc_lost_power_time_set_to_compile_time"));
  }

  loadConfig();
  servoLoadRange();
  // Do NOT park the flap at boot. Holding a stalled servo against its end stop
  // is peak current draw and it dragged the 5V rail down hard enough to brownout
  // and reset the UNO (symptom: serial output garbles, commands stop answering).
  // Stay detached until a feed actually needs the horn; `soff`/`son` still work.
  flap.detach();

  // doReset=false: this library's reset() ends in read(), which blocks forever
  // if the HX711 is absent. is_ready() is just DOUT going low, and the chip's
  // power-on rate is 10Hz (one conversion ~100ms), so the window must be well
  // over 100ms or the check times out on a perfectly healthy module.
  scale.begin(DOUT_PIN, SCK_PIN, true, false);
  scaleReady = scale.wait_ready_timeout(2000, 20);
  if (!scaleReady) {
    Serial.println(F("WARN,no_scale_boot_continues"));
  } else {
    scale.set_scale(calValue);
    scale.tare();
  }

  beep(1, 100, 0);
  showMessage("PetWatch Ready", 3000);
  Serial.println(F("PetWatch Ready"));
  Serial.print(F("portion="));
  Serial.println(cfg.portion);
  Serial.print(F("mealA="));
  Serial.print(cfg.hourA);
  Serial.print(F(":"));
  Serial.println(cfg.minA);
  Serial.print(F("mealB="));
  Serial.print(cfg.hourB);
  Serial.print(F(":"));
  Serial.println(cfg.minB);
}

void loop() {
  ensureRTC();
  servoSweepTick();
  handleManualButton();
  if (millis() - lastWeight >= 300) {
    lastWeight = millis();
    lastGrams = readGrams();
  }

  handleSerial();
  checkSchedule();
  checkLowCross();
  checkMissedMeal();
  updateDisplay();
}