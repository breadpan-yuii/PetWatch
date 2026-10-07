/*
 * Stage 2 test: HX711 + load cell raw readout.
 * Wire: HX711 VCC=5V GND=GND DOUT=D3 SCK=D2.
 * Serial (9600): prints raw + grams every 500ms.
 * Commands:  z        -> zero with empty bowl on platform
 *            c <grams> -> calibrate: put that weight on, then "c 100"
 */

#include <HX711.h>

HX711 scale;

float    factor = 1.0f;
char     cmd[40];
uint8_t  cmdLen = 0;
long     raw    = 0;

void setup() {
  Serial.begin(9600);
  scale.begin(3, 2);
  scale.tare();
  Serial.println("HX711 raw readout running; commands: z, c <grams>");
}

void loop() {
  raw = scale.get_value(5);
  Serial.print("raw=");
  Serial.print(raw);
  Serial.print("  grams=");
  Serial.println(raw / factor, 1);

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      cmd[cmdLen] = '\0';
      if (cmd[0] == 'z') {
        scale.tare();
        Serial.println("OK,z");
      } else if (strncmp(cmd, "c ", 2) == 0) {
        int known = atoi(cmd + 2);
        if (known > 0) {
          long r = scale.get_value(10);
          factor = (float)r / (float)known;
          Serial.print("OK,cal=");
          Serial.println(factor, 6);
        }
      }
      cmdLen = 0;
    } else if (cmdLen < sizeof(cmd) - 1) {
      cmd[cmdLen++] = c;
    }
  }

  delay(500);
}