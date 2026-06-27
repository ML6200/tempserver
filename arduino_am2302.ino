#include "DHT.h"

#define DHTPIN  2       // DATA connected to D2
#define DHTTYPE DHT22   // AM2302 = DHT22

DHT dht(DHTPIN, DHTTYPE);

void setup() {
  Serial.begin(9600);
  pinMode(DHTPIN, INPUT_PULLUP);   // internal pull-up; AM2302 reads reliably without an external resistor
  dht.begin();
}

void loop() {
  delay(2000);  // AM2302 needs ~2s between reads

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
