/*
 * Stage 3 test: servo sweep between closed (15) and open (95).
 * Wire: servo signal D9, 5V, GND. Flap should swing closed<->open.
 * Send any line in Serial (9600) to toggle; 'c'/'o' sets closed/open.
 */

#include <Servo.h>

Servo flap;

const uint8_t SERVO_CLOSED = 15;
const uint8_t SERVO_OPEN   = 95;

char     cmd[40];
uint8_t  cmdLen = 0;
uint8_t  pos    = SERVO_CLOSED;

void setup() {
  Serial.begin(9600);
  flap.attach(9);
  flap.write(SERVO_CLOSED);
  Serial.println("Servo at CLOSED(15). Send c / o / <anything toggles>");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      cmd[cmdLen] = '\0';
      cmdLen = 0;
      if (cmd[0] == 'c')      pos = SERVO_CLOSED;
      else if (cmd[0] == 'o') pos = SERVO_OPEN;
      else                    pos = (pos == SERVO_OPEN) ? SERVO_CLOSED : SERVO_OPEN;
      flap.write(pos);
      Serial.print("servo=");
      Serial.println(pos);
    } else if (cmdLen < sizeof(cmd) - 1) {
      cmd[cmdLen++] = c;
    }
  }
  delay(10);
}