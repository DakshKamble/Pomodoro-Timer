#include <Arduino.h>
#include <FastLED.h>
#include <Wire.h>
#include <U8g2lib.h>

// ── NeoPixel Config ──────────────────────────────────────────
#define NUM_LEDS      12
#define LED_PIN       D0
#define BRIGHTNESS    25

// ── Encoder Config ───────────────────────────────────────────
#define ENC_CLK       D1
#define ENC_DT        D2
#define ENC_SW        D3

// ── Tuning ───────────────────────────────────────────────────
#define COUNTS_PER_REV   80    // Adjust until one full rotation = full ring
#define FADE_DURATION_MS 300   // How long a new LED takes to fully fade in

// ── OLED Config ──────────────────────────────────────────────
// Changed to SSD1306 to fix the interleaved black lines issue.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);


// ── State ────────────────────────────────────────────────────
CRGB leds[NUM_LEDS];

volatile bool buttonPressed = false;
volatile int  encoderValue  = 0;

// Per-LED brightness targets and current rendered brightness
uint8_t targetBrightness[NUM_LEDS];   // 0 or 255 — what each LED should be
uint8_t currentBrightness[NUM_LEDS];  // actual rendered brightness (fading toward target)
unsigned long fadeStartTime[NUM_LEDS];// when this LED's current fade began
uint8_t fadeStartBrightness[NUM_LEDS];// brightness at the moment fade started

// ── Encoder ISR ──────────────────────────────────────────────
void IRAM_ATTR onEncoderTick() {
    static uint8_t lastState = 0b11;

    uint8_t clk   = digitalRead(ENC_CLK);
    uint8_t dt    = digitalRead(ENC_DT);
    uint8_t state = (clk << 1) | dt;

    if (state == lastState) return;

    if ((lastState == 0b11 && state == 0b01) ||
        (lastState == 0b01 && state == 0b00) ||
        (lastState == 0b00 && state == 0b10) ||
        (lastState == 0b10 && state == 0b11)) {
        encoderValue++;
    } else if ((lastState == 0b11 && state == 0b10) ||
               (lastState == 0b10 && state == 0b00) ||
               (lastState == 0b00 && state == 0b01) ||
               (lastState == 0b01 && state == 0b11)) {
        encoderValue--;
    }

    lastState = state;
}

// ── Button ISR ───────────────────────────────────────────────
void IRAM_ATTR onButtonPress() {
    static unsigned long lastPress = 0;
    unsigned long now = millis();
    if (now - lastPress < 200) return;
    lastPress = now;
    buttonPressed = true;
}

// ── Helpers ──────────────────────────────────────────────────

// Call when a LED's target changes — kicks off a new fade from wherever it is now
void startFade(int i, uint8_t newTarget) {
    if (targetBrightness[i] == newTarget) return; // already heading there
    targetBrightness[i]    = newTarget;
    fadeStartBrightness[i] = currentBrightness[i]; // start from current position
    fadeStartTime[i]       = millis();
}

// Ease-in-out curve (cubic): 0.0 → 1.0
float easeInOut(float t) {
    return t * t * (3.0f - 2.0f * t);
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);

    pinMode(ENC_CLK, INPUT_PULLUP);
    pinMode(ENC_DT,  INPUT_PULLUP);
    pinMode(ENC_SW,  INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(ENC_CLK), onEncoderTick, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_DT),  onEncoderTick, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_SW),  onButtonPress, FALLING);

    // Initialize OLED Display
    u8g2.setI2CAddress(0x78); 
    u8g2.begin();
    u8g2.setBusClock(400000); 

    // Init all LEDs off
    for (int i = 0; i < NUM_LEDS; i++) {
        targetBrightness[i]    = 0;
        currentBrightness[i]   = 0;
        fadeStartBrightness[i] = 0;
        fadeStartTime[i]       = 0;
    }
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
    // Snapshot volatile state
    noInterrupts();
    int  val     = encoderValue;
    bool pressed = buttonPressed;
    if (buttonPressed) buttonPressed = false;
    interrupts();

    // Clamp encoder so it never goes below 0 or above full ring
    val = constrain(val, 0, COUNTS_PER_REV);
    noInterrupts();
    encoderValue = val;
    interrupts();

    // Button: instant full white flash, then restore
    if (pressed) {
        Serial.println("Button clicked!");
        fill_solid(leds, NUM_LEDS, CRGB::White);
        FastLED.show();
        
        // Show indicator on OLED briefly
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB14_tr);
        u8g2.drawStr(10, 35, "CLICKED!");
        u8g2.sendBuffer();
        
        delay(200);
    }

    // How many LEDs should be fully ON based on encoder position
    int litLeds = map(val, 0, COUNTS_PER_REV, 0, NUM_LEDS);

    // Update fade targets — only trigger startFade() when target actually changes
    for (int i = 0; i < NUM_LEDS; i++) {
        uint8_t desired = (i < litLeds) ? 255 : 0;
        startFade(i, desired);
    }

    // Advance each LED's fade
    unsigned long now = millis();
    for (int i = 0; i < NUM_LEDS; i++) {
        if (currentBrightness[i] == targetBrightness[i]) continue; // settled

        unsigned long elapsed = now - fadeStartTime[i];

        if (elapsed >= FADE_DURATION_MS) {
            // Fade complete — snap to target
            currentBrightness[i] = targetBrightness[i];
        } else {
            float t = (float)elapsed / (float)FADE_DURATION_MS;
            float eased = easeInOut(t);
            currentBrightness[i] = (uint8_t)(
                fadeStartBrightness[i] + eased * ((int)targetBrightness[i] - (int)fadeStartBrightness[i])
            );
        }

        leds[i] = CRGB(0, currentBrightness[i], 0); // Green, brightness-scaled
    }

    FastLED.show();

    // Throttled serial & OLED updates (Max 5 FPS to keep animations smooth)
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 200) {
        // Serial Debug
        Serial.print("Encoder: "); Serial.print(val);
        Serial.print("   Lit: ");   Serial.println(litLeds);

        // OLED Update
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB10_tr); // Standard crisp font
        
        // Line 1: Encoder Value
        u8g2.setCursor(0, 20);
        u8g2.print("Encoder: "); 
        u8g2.print(val);
        
        // Line 2: Lit LEDs
        u8g2.setCursor(0, 45);
        u8g2.print("Lit LEDs: "); 
        u8g2.print(litLeds);
        
        // Progress Bar showing percentage of ring lit
        int barWidth = map(val, 0, COUNTS_PER_REV, 0, 128);
        u8g2.drawFrame(0, 55, 128, 9);
        u8g2.drawBox(0, 55, barWidth, 9);

        u8g2.sendBuffer();

        lastPrint = millis();
    }

    delay(10);
}