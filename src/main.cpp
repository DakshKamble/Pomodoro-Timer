#include <Arduino.h>
#include <FastLED.h>

// NeoPixel Config
#define NUM_LEDS 12
#define LED_PIN D0
#define BRIGHTNESS 25

CRGB leds[NUM_LEDS];

// Encoder Config
#define ENC_CLK D1
#define ENC_DT D2
#define ENC_SW D3

volatile bool buttonPressed = false;
volatile int encoderValue = 0;

// Interrupt --- calling on every falling edge of CLK

void IRAM_ATTR onEncoderTick() {
    if(digitalRead(ENC_DT) == HIGH) {
        encoderValue++; // Clockwise
    } else {
        encoderValue--; // Anticlockwise
    }
}

void IRAM_ATTR onButtonPress() {
    buttonPressed = true;
}

//////// SETUP /////////////////////////////////

void setup() {
    Serial.begin(115200);

    FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);

    pinMode(ENC_CLK, INPUT_PULLUP);
    pinMode(ENC_DT, INPUT_PULLUP);
    pinMode(ENC_SW, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(ENC_CLK), onEncoderTick, FALLING);
    attachInterrupt(digitalPinToInterrupt(ENC_SW), onButtonPress, FALLING);
}

//////// LOOP ////////////////////////////////////////
void loop() {
    // Snapshot volatile state safely
    noInterrupts();
    int  val     = encoderValue;
    bool pressed = buttonPressed;
    if (buttonPressed) buttonPressed = false;
    interrupts();

    if (pressed) {
        Serial.println("Button clicked!");
        fill_solid(leds, NUM_LEDS, CRGB::White);
        FastLED.show();
        delay(200);
    }

    // Map encoder to LED count (0–12), clamped
    int litLeds = constrain(val, 0, NUM_LEDS);
    for (int i = 0; i < NUM_LEDS; i++) {
        leds[i] = (i < litLeds) ? CRGB::Green : CRGB::Black;
    }
    FastLED.show();

    Serial.print("Encoder: "); Serial.println(val);
    delay(50);
}