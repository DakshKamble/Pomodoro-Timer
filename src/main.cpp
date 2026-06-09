#include <Arduino.h>
#include <FastLED.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <esp_sleep.h> // ESP32 Sleep Library
#include <Preferences.h> // ESP32 Non-Volatile Storage Library

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
#define MAX_SECONDS      14400 // Maximum timer duration (4 Hours = 240 * 60)
#define INCREMENT_SECONDS 60   // Increment by exactly 1 minute
#define SLEEP_TIMEOUT_MS  10000 // 10 seconds of inactivity before deep sleep
#define LONG_PRESS_MS     800  // Milliseconds required to trigger a long press

// ── Encoder Debounce/Sensitivity Margin ──────────────────────
#define PULSES_PER_ACTION 4   

// ── OLED Config ──────────────────────────────────────────────
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ── State Machine ────────────────────────────────────────────
enum TimerState { SETTING, RUNNING, FINISHED, CONFIG_DEFAULT_TIME };
TimerState currentState = SETTING;

unsigned long timerStartTime = 0;
unsigned long timerDurationMs = 0;
unsigned long timeRemainingMs = 0;

// Track the setting phase colors and timeouts
CRGB currentSettingColor = CRGB(0, 0, 0); 
CRGB targetSettingColor  = CRGB(0, 0, 0); 
unsigned long lastSettingChangeTime = 0;
int lastSettingVal = 0;

// Track inactivity for deep sleep
unsigned long lastActivityTime = 0;

// Storage
Preferences preferences;
unsigned int defaultSeconds = 1500; // 25 mins default, overridden by Preferences

// RTC memory variables survive Deep Sleep resets
RTC_DATA_ATTR int rtcSavedSeconds = 1500;

// ── Globals ──────────────────────────────────────────────────
CRGB leds[NUM_LEDS];

volatile bool shortPressFlag = false;
volatile bool longPressFlag  = false;
volatile int  rawPulses      = 0;
volatile int  encoderSeconds = 1500;

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

    // Accumulate raw state changes
    if ((lastState == 0b11 && state == 0b01) ||
        (lastState == 0b01 && state == 0b00) ||
        (lastState == 0b00 && state == 0b10) ||
        (lastState == 0b10 && state == 0b11)) {
        rawPulses++;
    } else if ((lastState == 0b11 && state == 0b10) ||
               (lastState == 0b10 && state == 0b00) ||
               (lastState == 0b00 && state == 0b01) ||
               (lastState == 0b01 && state == 0b11)) {
        rawPulses--;
    }

    lastState = state;

    // Only apply the change if we have accumulated enough valid pulses
    if (rawPulses >= PULSES_PER_ACTION) {
        encoderSeconds += INCREMENT_SECONDS;
        rawPulses = 0;
    } else if (rawPulses <= -PULSES_PER_ACTION) {
        encoderSeconds -= INCREMENT_SECONDS;
        rawPulses = 0;
    }
}

// ── Button ISR ───────────────────────────────────────────────
void IRAM_ATTR onButtonStateChange() {
    static unsigned long pressTime = 0;
    bool isPressed = (digitalRead(ENC_SW) == LOW);
    
    if (isPressed) {
        pressTime = millis();
    } else {
        if (pressTime > 0) {
            unsigned long duration = millis() - pressTime;
            if (duration >= LONG_PRESS_MS) {
                longPressFlag = true;
            } else if (duration > 50) { // 50ms Debounce for short press
                shortPressFlag = true;
            }
            pressTime = 0;
        }
    }
}

// ── Deep Sleep Helper ────────────────────────────────────────
void enterDeepSleep() {
    Serial.println("Going to sleep...");

    // Save current setting to RTC memory so it remembers when it wakes up
    rtcSavedSeconds = encoderSeconds;

    // Turn off LEDs
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();

    // Turn off OLED display safely
    u8g2.setPowerSave(1);

    // Configure the button pin (ENC_SW) as the wake-up source for ESP32-C3
    pinMode(ENC_SW, INPUT_PULLUP);
    esp_deep_sleep_enable_gpio_wakeup(1ULL << ENC_SW, ESP_GPIO_WAKEUP_GPIO_LOW);

    // Enter deep sleep indefinitely
    esp_deep_sleep_start();
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

    // Load default timer value from permanent memory (Preferences)
    preferences.begin("pomodoro", false);
    defaultSeconds = preferences.getUInt("defaultSec", 1500); // 1500s = 25m fallback

    // Wait until the button is RELEASED before continuing. 
    // This stops the "wake up" button press from registering instantly.
    pinMode(ENC_SW,  INPUT_PULLUP);
    while (digitalRead(ENC_SW) == LOW) {
        delay(10);
    }

    // If we woke up from deep sleep, restore exact last value. 
    // If hard boot, load the saved user default.
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
        encoderSeconds = rtcSavedSeconds;
    } else {
        encoderSeconds = defaultSeconds;
    }
    lastSettingVal = encoderSeconds;

    FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);

    pinMode(ENC_CLK, INPUT_PULLUP);
    pinMode(ENC_DT,  INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(ENC_CLK), onEncoderTick, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_DT),  onEncoderTick, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_SW),  onButtonStateChange, CHANGE); // Note: Changed to CHANGE for duration tracking

    u8g2.setI2CAddress(0x78);
    u8g2.begin();
    u8g2.setBusClock(400000);

    for (int i = 0; i < NUM_LEDS; i++) {
        targetBrightness[i]    = 0;
        currentBrightness[i]   = 0;
        fadeStartBrightness[i] = 0;
        fadeStartTime[i]       = 0;
    }

    lastActivityTime = millis(); // Reset activity timer on boot
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
    // Snapshot volatile state safely
    noInterrupts();
    int  val         = encoderSeconds;
    bool shortPress  = shortPressFlag;
    bool longPress   = longPressFlag;
    if (shortPressFlag) shortPressFlag = false;
    if (longPressFlag)  longPressFlag  = false;
    interrupts();

    // Reset inactivity timer if the user does anything
    static int lastActivityVal = val;
    if (shortPress || longPress || val != lastActivityVal) {
        lastActivityTime = millis();
        lastActivityVal = val;
    }

    // Check for Deep Sleep condition (10s of inactivity while NOT running)
    if ((currentState == SETTING || currentState == FINISHED || currentState == CONFIG_DEFAULT_TIME) 
        && (millis() - lastActivityTime > SLEEP_TIMEOUT_MS)) {
        enterDeepSleep();
    }

    // ── STATE MACHINE LOGIC ──
    if (currentState == SETTING) {
        val = constrain(val, INCREMENT_SECONDS, MAX_SECONDS);
        noInterrupts(); encoderSeconds = val; interrupts();

        // Trigger smooth Green/Red flashes based on turn direction
        if (val > lastSettingVal) {
            targetSettingColor = CRGB(0, 255, 0); // Target Green
            lastSettingChangeTime = millis();
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 255);
        } else if (val < lastSettingVal) {
            targetSettingColor = CRGB(255, 0, 0); // Target Red
            lastSettingChangeTime = millis();
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 255);
        }
        lastSettingVal = val;

        if (millis() - lastSettingChangeTime > 300) {
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 0);
        }

        if (shortPress) {
            currentState = RUNNING;
            timerDurationMs = (unsigned long)val * 1000UL; 
            timerStartTime = millis();
            timeRemainingMs = timerDurationMs;
        } else if (longPress) {
            currentState = CONFIG_DEFAULT_TIME;
            noInterrupts(); encoderSeconds = defaultSeconds; interrupts(); // Load current default into encoder
            lastSettingVal = defaultSeconds;
        }
    } 
    else if (currentState == CONFIG_DEFAULT_TIME) {
        val = constrain(val, INCREMENT_SECONDS, MAX_SECONDS);
        noInterrupts(); encoderSeconds = val; interrupts();

        // Use Blue/Purple to indicate we are changing the default
        if (val > lastSettingVal) {
            targetSettingColor = CRGB(0, 100, 255); // Blue for increase
            lastSettingChangeTime = millis();
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 255);
        } else if (val < lastSettingVal) {
            targetSettingColor = CRGB(150, 0, 255); // Purple for decrease
            lastSettingChangeTime = millis();
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 255);
        }
        lastSettingVal = val;

        if (millis() - lastSettingChangeTime > 300) {
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 0);
        }

        // Any press saves the new default and exits back to Setting mode
        if (shortPress || longPress) {
            defaultSeconds = val;
            preferences.putUInt("defaultSec", defaultSeconds); // Save to Non-Volatile Memory
            
            currentState = SETTING;
            noInterrupts(); encoderSeconds = defaultSeconds; interrupts();
            lastSettingVal = defaultSeconds;
        }
    }
    else if (currentState == RUNNING) {
        if (shortPress) {
            // Cancel timer
            currentState = SETTING;
            lastActivityTime = millis();
            noInterrupts(); encoderSeconds = val; interrupts();
        } else {
            unsigned long elapsed = millis() - timerStartTime;
            if (elapsed >= timerDurationMs) {
                currentState = FINISHED;
                timerStartTime = millis(); 
                lastActivityTime = millis(); 
            } else {
                timeRemainingMs = timerDurationMs - elapsed;
            }
        }
    }
    else if (currentState == FINISHED) {
        if (shortPress || longPress) {
            currentState = SETTING;
            lastActivityTime = millis();
        }
    }

    // ── LED TARGET LOGIC (Running & Finished) ──
    int litLeds = 0;
    if (currentState == RUNNING) {
        // Map remaining time to LEDs
        litLeds = (timeRemainingMs * NUM_LEDS + timerDurationMs - 1) / timerDurationMs;
    }

    if (currentState != SETTING && currentState != CONFIG_DEFAULT_TIME) {
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

    // ── SMOOTH COLOR BLEND (For Setting Modes) ──
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
        if (currentState == SETTING || currentState == CONFIG_DEFAULT_TIME) {
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

        if (currentState == SETTING || currentState == CONFIG_DEFAULT_TIME) {
            u8g2.setFont(u8g2_font_ncenB10_tr);
            if (currentState == CONFIG_DEFAULT_TIME) {
                u8g2.drawStr(0, 15, "Set Default:");
            } else {
                u8g2.drawStr(0, 15, "Set Timer:");
            }
            
            int h = val / 3600;
            int m = (val % 3600) / 60;
            int s = val % 60;
            
            u8g2.setFont(u8g2_font_ncenB18_tr); 
            sprintf(timeBuf, "%d:%02d:%02d", h, m, s);
            u8g2.drawStr(12, 50, timeBuf);
        } 
        else if (currentState == RUNNING) {
            u8g2.setFont(u8g2_font_ncenB10_tr);
            u8g2.drawStr(0, 15, "Focusing:");

            unsigned long secsRemaining = timeRemainingMs / 1000;
            int h = secsRemaining / 3600;
            int m = (secsRemaining % 3600) / 60;
            int s = secsRemaining % 60;

            u8g2.setFont(u8g2_font_ncenB18_tr);
            sprintf(timeBuf, "%d:%02d:%02d", h, m, s);
            u8g2.drawStr(12, 48, timeBuf);

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