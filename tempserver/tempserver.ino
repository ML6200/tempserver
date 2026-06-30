#include "DHT.h"

#define DHTPIN  2       // DATA connected to D2
#define DHTTYPE DHT22   // AM2302 = DHT22
#define FANPIN  9       // PWM to the fan transistor (flyback-protected)

DHT dht(DHTPIN, DHTTYPE);

// Fan state. The server sends "F<duty>\n" (duty 0-255) over serial; we apply it
// with analogWrite. If the server goes silent for FAN_WATCHDOG_MS (crash, cable
// yank) we fail safe to full speed, since a stuck-off fan is the dangerous case.
const unsigned long FAN_WATCHDOG_MS = 10000;
unsigned long lastCmdMs = 0;
int fanDuty = 0;

void setFan(int duty) {
  if (duty < 0) duty = 0;
  if (duty > 255) duty = 255;
  fanDuty = duty;
  analogWrite(FANPIN, fanDuty);
}

// Drain any pending "F<number>\n" commands and apply the last one.
void handleSerial() {
  static int val = 0;
  static bool inCmd = false;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'F') {            // start of a command
      inCmd = true;
      val = 0;
    } else if (inCmd && c >= '0' && c <= '9') {
      val = val * 10 + (c - '0');
    } else if (c == '\n' || c == '\r') {
      if (inCmd) {
        setFan(val);
        lastCmdMs = millis();
        inCmd = false;
      }
    } else {                  // stray byte -> abandon the partial command
      inCmd = false;
    }
  }
}

void setup() {
  Serial.begin(9600);
  pinMode(DHTPIN, INPUT_PULLUP);   // internal pull-up; AM2302 reads reliably without an external resistor
  pinMode(FANPIN, OUTPUT);
  setFan(0);                       // fan off until the server commands otherwise
  dht.begin();
  lastCmdMs = millis();
}

void loop() {
  // AM2302 needs ~2s between reads. Spend that interval servicing fan commands
  // instead of blocking, so PWM updates stay responsive and we never let the
  // serial RX buffer overflow.
  unsigned long start = millis();
  while (millis() - start < 2000) {
    handleSerial();
    if (millis() - lastCmdMs > FAN_WATCHDOG_MS && fanDuty != 255)
      setFan(255);                 // server silent -> fail safe to full speed
    delay(5);
  }

  float h = dht.readHumidity();
  float t = dht.readTemperature();   // Celsius

  if (isnan(h) || isnan(t)) {
    Serial.println("nan,nan");       // sensor read failed -> server records a sensor error
    return;
  }

  // Clean machine-readable line: humidity,temperature
  Serial.print(h, 2);
  Serial.print(',');
  Serial.println(t, 2);
}
