#include <Arduino.h>
#include <FastLED.h>

// ── NeoPixel Config ──────────────────────────────────────────
#define NUM_LEDS   12
#define LED_PIN    D0
#define BRIGHTNESS 25

CRGB leds[NUM_LEDS];

// ── Encoder Config ───────────────────────────────────────────
#define ENC_CLK D1
#define ENC_DT  D2
#define ENC_SW  D3

volatile bool buttonPressed = false;
volatile int  encoderValue  = 0;

// ── Encoder ISR (quadrature state machine, both edges) ───────
// Fires on CHANGE for both CLK and DT.
// Valid CW  sequence: 00→10→11→01→00
// Valid CCW sequence: 00→01→11→10→00
void IRAM_ATTR onEncoderTick() {
    static uint8_t lastState = 0b11; // pullups → idle state is HIGH,HIGH

    uint8_t clk   = digitalRead(ENC_CLK);
    uint8_t dt    = digitalRead(ENC_DT);
    uint8_t state = (clk << 1) | dt;

    if (state == lastState) return; // no real change (spurious)

    // Clockwise transitions
    if ((lastState == 0b11 && state == 0b01) ||
        (lastState == 0b01 && state == 0b00) ||
        (lastState == 0b00 && state == 0b10) ||
        (lastState == 0b10 && state == 0b11)) {
        encoderValue++;
    }
    // Counter-clockwise transitions
    else if ((lastState == 0b11 && state == 0b10) ||
             (lastState == 0b10 && state == 0b00) ||
             (lastState == 0b00 && state == 0b01) ||
             (lastState == 0b01 && state == 0b11)) {
        encoderValue--;
    }
    // Any other transition is noise — ignore it

    lastState = state;
}

// ── Button ISR (software debounce via time gate) ─────────────
void IRAM_ATTR onButtonPress() {
    static unsigned long lastPress = 0;
    unsigned long now = millis();
    if (now - lastPress < 200) return; // ignore bounces within 200 ms
    lastPress = now;
    buttonPressed = true;
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);

    pinMode(ENC_CLK, INPUT_PULLUP);
    pinMode(ENC_DT,  INPUT_PULLUP);
    pinMode(ENC_SW,  INPUT_PULLUP);

    // CHANGE on both pins for full quadrature decoding
    attachInterrupt(digitalPinToInterrupt(ENC_CLK), onEncoderTick, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_DT),  onEncoderTick, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_SW),  onButtonPress, FALLING);
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
    // Atomically snapshot volatile state
    noInterrupts();
    int  val     = encoderValue;
    bool pressed = buttonPressed;
    if (buttonPressed) buttonPressed = false;
    interrupts();

    // Button: flash white
    if (pressed) {
        Serial.println("Button clicked!");
        fill_solid(leds, NUM_LEDS, CRGB::White);
        FastLED.show();
        delay(200);
    }

    // Map encoder to LED bar (0–12), clamped
    int litLeds = constrain(val, 0, NUM_LEDS);
    for (int i = 0; i < NUM_LEDS; i++) {
        leds[i] = (i < litLeds) ? CRGB::Green : CRGB::Black;
    }
    FastLED.show();

    // Throttled serial — won't pollute the loop timing
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 100) {
        Serial.print("Encoder: "); Serial.println(val);
        lastPrint = millis();
    }

    delay(10); // shorter delay = more responsive LED updates
}