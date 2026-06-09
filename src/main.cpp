#include <Arduino.h>
#include <FastLED.h>

// NeoPixel Config
#define NUM_LEDS 12
#define LED_PIN D0
#define BRIGHTNESS 25

CRGB leds[NUM_LEDS];

void setup() {
    FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);
}

void loop() {
  // Red
  fill_solid(leds, NUM_LEDS, CRGB::Red);
  FastLED.show();
  delay(100);

  // Green
  fill_solid(leds, NUM_LEDS, CRGB::Green);
  FastLED.show();
  delay(100);

  // Blue
  fill_solid(leds, NUM_LEDS, CRGB::Blue);
  FastLED.show();
  delay(100);
}