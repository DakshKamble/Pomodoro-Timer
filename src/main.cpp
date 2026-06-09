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
#define FADE_DURATION_MS 300   // How long a new LED takes to fully fade in/out
#define MAX_MINUTES      240   // Maximum timer duration (4 Hours)
#define DEFAULT_MINUTES  25    // Classic Pomodoro length

// ── OLED Config ──────────────────────────────────────────────
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ── State Machine ────────────────────────────────────────────
enum TimerState { SETTING, RUNNING, FINISHED };
TimerState currentState = SETTING;

unsigned long timerStartTime = 0;
unsigned long timerDurationMs = 0;
unsigned long timeRemainingMs = 0;

// Track the setting phase colors and timeouts
CRGB currentSettingColor = CRGB(0, 0, 0); // The color currently being rendered
CRGB targetSettingColor  = CRGB(0, 0, 0); // The color we want to morph into
unsigned long lastSettingChangeTime = 0;
int lastSettingVal = DEFAULT_MINUTES;

// ── Globals ──────────────────────────────────────────────────
CRGB leds[NUM_LEDS];

volatile bool buttonPressed = false;
volatile int  encoderValue  = DEFAULT_MINUTES;

// Per-LED brightness targets and current rendered brightness
uint8_t targetBrightness[NUM_LEDS];   
uint8_t currentBrightness[NUM_LEDS];  
unsigned long fadeStartTime[NUM_LEDS];
uint8_t fadeStartBrightness[NUM_LEDS];

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
    if (now - lastPress < 250) return; // Debounce
    lastPress = now;
    buttonPressed = true;
}

// ── Helpers ──────────────────────────────────────────────────
void startFade(int i, uint8_t newTarget) {
    if (targetBrightness[i] == newTarget) return; 
    targetBrightness[i]    = newTarget;
    fadeStartBrightness[i] = currentBrightness[i]; 
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

    u8g2.setI2CAddress(0x78);
    u8g2.begin();
    u8g2.setBusClock(400000);

    for (int i = 0; i < NUM_LEDS; i++) {
        targetBrightness[i]    = 0;
        currentBrightness[i]   = 0;
        fadeStartBrightness[i] = 0;
        fadeStartTime[i]       = 0;
    }
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
    // Snapshot volatile state safely
    noInterrupts();
    int  val     = encoderValue;
    bool pressed = buttonPressed;
    if (buttonPressed) buttonPressed = false;
    interrupts();

    // ── STATE MACHINE LOGIC ──
    if (currentState == SETTING) {
        // Clamp minutes between 1 and MAX_MINUTES
        val = constrain(val, 1, MAX_MINUTES);
        noInterrupts();
        encoderValue = val;
        interrupts();

        // Trigger smooth Green/Red flashes based on turn direction
        if (val > lastSettingVal) {
            targetSettingColor = CRGB(0, 255, 0); // Target Green for increase
            lastSettingChangeTime = millis();
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 255);
        } else if (val < lastSettingVal) {
            targetSettingColor = CRGB(255, 0, 0); // Target Red for decrease
            lastSettingChangeTime = millis();
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 255);
        }
        lastSettingVal = val;

        // Smoothly fade out to black if the encoder stops moving
        if (millis() - lastSettingChangeTime > 300) {
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 0);
        }

        if (pressed) {
            currentState = RUNNING;
            timerDurationMs = (unsigned long)val * 60000UL;
            timerStartTime = millis();
            timeRemainingMs = timerDurationMs;
        }
    } 
    else if (currentState == RUNNING) {
        if (pressed) {
            // Cancel timer
            currentState = SETTING;
            noInterrupts();
            encoderValue = val; // restore encoder back to last set value
            interrupts();
        } else {
            unsigned long elapsed = millis() - timerStartTime;
            if (elapsed >= timerDurationMs) {
                currentState = FINISHED;
                timerStartTime = millis(); // Reset start time for blink animation
            } else {
                timeRemainingMs = timerDurationMs - elapsed;
            }
        }
    }
    else if (currentState == FINISHED) {
        if (pressed) {
            currentState = SETTING;
        }
    }

    // ── LED TARGET LOGIC (Running & Finished) ──
    int litLeds = 0;
    if (currentState == RUNNING) {
        // Map remaining time to LEDs
        litLeds = (timeRemainingMs * NUM_LEDS + timerDurationMs - 1) / timerDurationMs;
    }

    // We only update targets here for RUNNING and FINISHED, 
    // because SETTING handles its own all-LED targets above.
    if (currentState != SETTING) {
        for (int i = 0; i < NUM_LEDS; i++) {
            uint8_t desired = 0;
            if (currentState == FINISHED) {
                // Blink all LEDs Red when done
                desired = ((millis() - timerStartTime) % 1000 < 500) ? 255 : 0;
            } else if (currentState == RUNNING) {
                desired = (i < litLeds) ? 255 : 0;
            }
            startFade(i, desired);
        }
    }

    // ── SMOOTH COLOR BLEND (For Setting Mode) ──
    // nblend morphs currentSettingColor towards targetSettingColor by a fraction (0-255).
    // 25 roughly equals a 10% step per frame, creating a rapid but smooth crossfade.
    nblend(currentSettingColor, targetSettingColor, 25);

    // ── FADE EXECUTION & COLOR MAPPING ──
    unsigned long now = millis();
    for (int i = 0; i < NUM_LEDS; i++) {
        if (currentBrightness[i] != targetBrightness[i]) {
            unsigned long elapsed = now - fadeStartTime[i];
            if (elapsed >= FADE_DURATION_MS) {
                currentBrightness[i] = targetBrightness[i];
            } else {
                float t = (float)elapsed / (float)FADE_DURATION_MS;
                float eased = easeInOut(t);
                currentBrightness[i] = (uint8_t)(
                    fadeStartBrightness[i] + eased * ((int)targetBrightness[i] - (int)fadeStartBrightness[i])
                );
            }
        }

        // Apply colors based on State
        if (currentState == SETTING) {
            // Apply the smoothly blended color, scaled by the individual LED's brightness fade
            leds[i] = CRGB(
                (currentSettingColor.r * currentBrightness[i]) / 255,
                (currentSettingColor.g * currentBrightness[i]) / 255,
                (currentSettingColor.b * currentBrightness[i]) / 255
            );
        } else if (currentState == RUNNING) {
            leds[i] = CRGB(0, currentBrightness[i], 0); // Green for Running
        } else {
            leds[i] = CRGB(currentBrightness[i], 0, 0); // Red for Finished
        }
    }

    FastLED.show();

    // ── OLED UPDATE LOGIC (Throttled to ~10 FPS) ──
    static unsigned long lastPrint = 0;
    if (now - lastPrint >= 100) {
        u8g2.clearBuffer();

        char timeBuf[16];

        if (currentState == SETTING) {
            u8g2.setFont(u8g2_font_ncenB10_tr);
            u8g2.drawStr(0, 15, "Set Timer:");
            
            u8g2.setFont(u8g2_font_ncenB24_tr);
            sprintf(timeBuf, "%02d:00", val);
            
            // Shift left slightly if it's over 99 mins to fit on screen
            if (val >= 100) {
                u8g2.drawStr(2, 55, timeBuf);
            } else {
                u8g2.drawStr(18, 55, timeBuf);
            }
        } 
        else if (currentState == RUNNING) {
            u8g2.setFont(u8g2_font_ncenB10_tr);
            u8g2.drawStr(0, 15, "Focusing:");

            unsigned long secsRemaining = timeRemainingMs / 1000;
            int m = secsRemaining / 60;
            int s = secsRemaining % 60;

            u8g2.setFont(u8g2_font_ncenB24_tr);
            sprintf(timeBuf, "%02d:%02d", m, s);
            
            // Shift left slightly if it's over 99 mins
            if (m >= 100) {
                u8g2.drawStr(2, 50, timeBuf); 
            } else {
                u8g2.drawStr(18, 50, timeBuf);
            }

            // Progress Bar
            int barWidth = map(timeRemainingMs, 0, timerDurationMs, 0, 128);
            u8g2.drawFrame(0, 58, 128, 6);
            u8g2.drawBox(0, 58, barWidth, 6);
        }
        else if (currentState == FINISHED) {
            u8g2.setFont(u8g2_font_ncenB14_tr);
            u8g2.drawStr(12, 40, "TIME'S UP!");
        }

        u8g2.sendBuffer();
        lastPrint = now;
    }

    delay(10); // Yield for stability
}