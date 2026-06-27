#include <Arduino.h>

const int LED_PIN = A0; // D13 і є PA5

// put function declarations here:
void myFunction(int pin);

void setup() {
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  myFunction(LED_PIN);
}

// put function definitions here:
void myFunction(int pin) {
  digitalWrite(pin, HIGH);
  delay(1000);
  digitalWrite(pin, LOW);
  delay(1000);
}