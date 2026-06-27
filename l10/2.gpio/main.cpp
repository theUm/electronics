#include <Arduino.h>

const int LED_RED = PB14;
const int LED_BLUE = PB7;

// put function declarations here:
void myFunction(int pin);

void setup() {
  pinMode(LED_RED, INPUT_PULLUP); 
  pinMode(LED_BLUE, INPUT_PULLDOWN); 
}

void loop() {
}
