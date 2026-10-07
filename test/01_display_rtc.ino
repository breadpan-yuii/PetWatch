/*
 * Stage 1 test: I2C scan, LCD hello, RTC time, buzzer check.
 * Wire: RTC + LCD on A4/A5, buzzer on D8, power via USB.
 * Serial (9600): prints scan results. Command: settime Y M D H M S
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

const uint8_t BUZZER_PIN = 8;
const uint8_t LCD_ADDR   = 0x27;   // try 0x3F if blank

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
RTC_DS3231        rtc;

char cmd[40];
uint8_t cmdLen = 0;

void beepBlip(uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(120);
  }
}

void setup() {
  Serial.begin(9600);
  pinMode(BUZZER_PIN, OUTPUT);
  lcd.init();
  lcd.backlight();

  byte err, addr;
  Serial.println("I2C scan:");
  for (addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    err = Wire.endTransmission();
    if (err == 0) {
      Serial.print("  found 0x");
      Serial.println(addr, HEX);
    }
  }

  if (rtc.begin()) {
    Serial.println("RTC OK");
    lcd.setCursor(0, 0);
    lcd.print("RTC FOUND    ");
  } else {
    Serial.println("RTC FAIL");
    lcd.setCursor(0, 0);
    lcd.print("RTC FAIL     ");
  }

  lcd.setCursor(0, 1);
  lcd.print("BOOT BEEP 3x  ");
  beepBlip(3);
}

void loop() {
  DateTime now = rtc.now();
  Serial.print(now.timestamp(DateTime::TIMESTAMP_FULL));
  Serial.print("  buzzer-last-test-done");
  Serial.println();

  char t[17];
  snprintf(t, sizeof(t), "%02u:%02u:%02u", now.hour(), now.minute(), now.second());
  lcd.setCursor(0, 1);
  for (uint8_t i = 0; i < 16; i++) lcd.print((i < strlen(t)) ? t[i] : ' ');

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      cmd[cmdLen] = '\0';
      if (strncmp(cmd, "settime", 7) == 0) {
        int y, mo, d, h, mi, se;
        if (sscanf(cmd + 7, "%d %d %d %d %d %d", &y, &mo, &d, &h, &mi, &se) == 6) {
          rtc.adjust(DateTime(y, mo, d, h, mi, se));
          Serial.println("OK,time set");
          beepBlip(1);
        } else {
          Serial.println("ERR,usage: settime Y M D H M S");
        }
      }
      cmdLen = 0;
    } else if (cmdLen < sizeof(cmd) - 1) {
      cmd[cmdLen++] = c;
    }
  }

  delay(500);
}