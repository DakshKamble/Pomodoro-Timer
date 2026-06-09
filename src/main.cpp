#include <Arduino.h>
#include <FastLED.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <Preferences.h>

// ── Adafruit Display Libraries ───────────────────────────────
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

// ── NeoPixel Config ──────────────────────────────────────────
#define NUM_LEDS      12
#define LED_PIN       D0
#define BRIGHTNESS    25

// ── Encoder Config ───────────────────────────────────────────
#define ENC_CLK       D1
#define ENC_DT        D2
#define ENC_SW        D3

// ── Tuning ───────────────────────────────────────────────────
#define FADE_DURATION_MS  300
#define MAX_SECONDS       14400
#define INCREMENT_SECONDS 60
#define SLEEP_TIMEOUT_MS  10000
#define LONG_PRESS_MS     800

// ── Encoder Debounce/Sensitivity ─────────────────────────────
#define PULSES_PER_ACTION 4

// ── OLED Config ──────────────────────────────────────────────
// 1.3 inch 128x64 SH1106 display
// SH1106 has 132 columns internally but displays 128.
// The Adafruit_SH1106G driver handles the 2-pixel column offset automatically.
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1  // No dedicated reset pin on XIAO / QT-PY

// ── FIX: Use Adafruit_SH1106G (NOT SSD1306) for 1.3" SH1106 displays ────────
// SSD1306 driver skips the 2-pixel column offset → causes warped/stretched output
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── State Machine ────────────────────────────────────────────
enum TimerState { SETTING, RUNNING, FINISHED, CONFIG_DEFAULT_TIME };
TimerState currentState = SETTING;

unsigned long timerStartTime   = 0;
unsigned long timerDurationMs  = 0;
unsigned long timeRemainingMs  = 0;

// Setting phase color tracking
CRGB currentSettingColor  = CRGB(0, 0, 0);
CRGB targetSettingColor   = CRGB(0, 0, 0);
unsigned long lastSettingChangeTime = 0;
int lastSettingVal = 0;

// Inactivity tracking for deep sleep
unsigned long lastActivityTime = 0;

// Non-volatile storage
Preferences preferences;
unsigned int defaultSeconds = 1500; // 25 min fallback

// RTC memory — survives deep sleep
RTC_DATA_ATTR int rtcSavedSeconds = 1500;

// ── Globals ──────────────────────────────────────────────────
CRGB leds[NUM_LEDS];

volatile bool shortPressFlag = false;
volatile bool longPressFlag  = false;
volatile int  rawPulses      = 0;
volatile int  encoderSeconds = 1500;

// Per-LED fade state
uint8_t targetBrightness[NUM_LEDS];
uint8_t currentBrightness[NUM_LEDS];
unsigned long fadeStartTime[NUM_LEDS];
uint8_t fadeStartBrightness[NUM_LEDS];

// ── Button press timestamps (set in ISR, evaluated in loop) ──
// FIX: Don't compute millis()-based duration inside ISR — millis() is unreliable
// inside interrupts on ESP32. Record timestamps only; evaluate in loop().
volatile unsigned long buttonPressStartMs   = 0;
volatile unsigned long buttonReleaseMs      = 0;
volatile bool          buttonReleaseFlag    = false;

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
        rawPulses++;
    } else if ((lastState == 0b11 && state == 0b10) ||
               (lastState == 0b10 && state == 0b00) ||
               (lastState == 0b00 && state == 0b01) ||
               (lastState == 0b01 && state == 0b11)) {
        rawPulses--;
    }

    lastState = state;

    if (rawPulses >= PULSES_PER_ACTION) {
        encoderSeconds += INCREMENT_SECONDS;
        rawPulses = 0;
    } else if (rawPulses <= -PULSES_PER_ACTION) {
        encoderSeconds -= INCREMENT_SECONDS;
        rawPulses = 0;
    }
}

// ── Button ISR ───────────────────────────────────────────────
// FIX: Only record timestamps here. Duration logic moved to loop().
void IRAM_ATTR onButtonStateChange() {
    bool isPressed = (digitalRead(ENC_SW) == LOW);
    if (isPressed) {
        buttonPressStartMs = millis();
    } else {
        buttonReleaseMs   = millis();
        buttonReleaseFlag = true;
    }
}

// ── Deep Sleep ───────────────────────────────────────────────
void enterDeepSleep() {
    Serial.println("Going to sleep...");

    rtcSavedSeconds = encoderSeconds; // Persist to RTC memory

    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();

    display.clearDisplay();
    display.display();
    display.oled_command(SH110X_DISPLAYOFF);

    pinMode(ENC_SW, INPUT_PULLUP);
    esp_deep_sleep_enable_gpio_wakeup(1ULL << ENC_SW, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

// ── LED Fade Helpers ─────────────────────────────────────────
void startFade(int i, uint8_t newTarget) {
    if (targetBrightness[i] == newTarget) return;
    targetBrightness[i]    = newTarget;
    fadeStartBrightness[i] = currentBrightness[i];
    fadeStartTime[i]       = millis();
}

float easeInOut(float t) {
    return t * t * (3.0f - 2.0f * t);
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    preferences.begin("pomodoro", false);
    defaultSeconds = preferences.getUInt("defaultSec", 1500);

    // Wait for button release before continuing (handles wake-from-sleep press)
    pinMode(ENC_SW, INPUT_PULLUP);
    while (digitalRead(ENC_SW) == LOW) {
        delay(10);
    }

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
    attachInterrupt(digitalPinToInterrupt(ENC_SW),  onButtonStateChange, CHANGE);

    // ── FIX: display.begin(addr, false) — pass false for reset ──────────────
    // With OLED_RESET = -1 (no physical reset pin), passing true triggers a
    // reset pulse on pin -1 which corrupts init and causes warped/stretched output.
    // The SH1106 needs: I2C addr 0x3C, no hardware reset.
    Wire.begin();
    Wire.setClock(400000);
    if (!display.begin(0x3C, false)) {
        Serial.println("SH1106 not found! Check wiring.");
        // Blink LED to signal error rather than hanging silently
        while (true) {
            fill_solid(leds, NUM_LEDS, CRGB::Red);
            FastLED.show();
            delay(300);
            fill_solid(leds, NUM_LEDS, CRGB::Black);
            FastLED.show();
            delay(300);
        }
    }

    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    display.setTextWrap(false); // Prevent text wrapping at screen edge
    display.display();

    for (int i = 0; i < NUM_LEDS; i++) {
        targetBrightness[i]    = 0;
        currentBrightness[i]   = 0;
        fadeStartBrightness[i] = 0;
        fadeStartTime[i]       = 0;
    }

    lastActivityTime = millis();
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
    // ── Snapshot volatile encoder state ──
    noInterrupts();
    int val = encoderSeconds;
    interrupts();

    // ── FIX: Evaluate button duration in loop(), not in ISR ──
    bool shortPress = false;
    bool longPress  = false;

    noInterrupts();
    bool released     = buttonReleaseFlag;
    unsigned long pStart = buttonPressStartMs;
    unsigned long pEnd   = buttonReleaseMs;
    if (released) buttonReleaseFlag = false;
    interrupts();

    if (shortPressFlag) { shortPress = true; shortPressFlag = false; } // legacy path unused now
    if (longPressFlag)  { longPress  = true; longPressFlag  = false; }

    if (released && pStart > 0) {
        unsigned long duration = pEnd - pStart;
        if (duration >= LONG_PRESS_MS) {
            longPress = true;
        } else if (duration >= 50) { // 50ms debounce
            shortPress = true;
        }
    }

    // ── Activity tracking ──
    static int lastActivityVal = val;
    if (shortPress || longPress || val != lastActivityVal) {
        lastActivityTime  = millis();
        lastActivityVal   = val;
    }

    // ── Deep Sleep check ──
    if ((currentState == SETTING || currentState == FINISHED || currentState == CONFIG_DEFAULT_TIME)
        && (millis() - lastActivityTime > SLEEP_TIMEOUT_MS)) {
        enterDeepSleep();
    }

    // ── STATE: SETTING ────────────────────────────────────────
    if (currentState == SETTING) {
        // FIX: Constrain BEFORE writing back, so encoderSeconds never holds OOB value
        val = constrain(val, INCREMENT_SECONDS, MAX_SECONDS);
        noInterrupts(); encoderSeconds = val; interrupts();

        if (val > lastSettingVal) {
            targetSettingColor    = CRGB(0, 255, 0);
            lastSettingChangeTime = millis();
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 255);
        } else if (val < lastSettingVal) {
            targetSettingColor    = CRGB(255, 0, 0);
            lastSettingChangeTime = millis();
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 255);
        }
        lastSettingVal = val;

        if (millis() - lastSettingChangeTime > 300) {
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 0);
        }

        if (shortPress) {
            currentState    = RUNNING;
            timerDurationMs = (unsigned long)val * 1000UL;
            timerStartTime  = millis();
            timeRemainingMs = timerDurationMs;
        } else if (longPress) {
            currentState = CONFIG_DEFAULT_TIME;
            noInterrupts(); encoderSeconds = defaultSeconds; interrupts();
            lastSettingVal = defaultSeconds;
        }
    }
    // ── STATE: CONFIG_DEFAULT_TIME ────────────────────────────
    else if (currentState == CONFIG_DEFAULT_TIME) {
        val = constrain(val, INCREMENT_SECONDS, MAX_SECONDS);
        noInterrupts(); encoderSeconds = val; interrupts();

        if (val > lastSettingVal) {
            targetSettingColor    = CRGB(0, 100, 255);
            lastSettingChangeTime = millis();
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 255);
        } else if (val < lastSettingVal) {
            targetSettingColor    = CRGB(150, 0, 255);
            lastSettingChangeTime = millis();
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 255);
        }
        lastSettingVal = val;

        if (millis() - lastSettingChangeTime > 300) {
            for (int i = 0; i < NUM_LEDS; i++) startFade(i, 0);
        }

        if (shortPress || longPress) {
            defaultSeconds = val;
            preferences.putUInt("defaultSec", defaultSeconds);

            currentState = SETTING;
            noInterrupts(); encoderSeconds = defaultSeconds; interrupts();
            lastSettingVal = defaultSeconds;
        }
    }
    // ── STATE: RUNNING ────────────────────────────────────────
    else if (currentState == RUNNING) {
        if (shortPress) {
            currentState = SETTING;
            lastActivityTime = millis();
            noInterrupts(); encoderSeconds = val; interrupts();
        } else {
            unsigned long elapsed = millis() - timerStartTime;
            if (elapsed >= timerDurationMs) {
                currentState   = FINISHED;
                timerStartTime = millis();
                lastActivityTime = millis();
            } else {
                timeRemainingMs = timerDurationMs - elapsed;
            }
        }
    }
    // ── STATE: FINISHED ───────────────────────────────────────
    else if (currentState == FINISHED) {
        if (shortPress || longPress) {
            currentState = SETTING;
            lastActivityTime = millis();
        }
    }

    // ── LED TARGET LOGIC ──────────────────────────────────────
    if (currentState != SETTING && currentState != CONFIG_DEFAULT_TIME) {
        int litLeds = 0;
        if (currentState == RUNNING) {
            litLeds = (int)((timeRemainingMs * NUM_LEDS + timerDurationMs - 1) / timerDurationMs);
        }

        for (int i = 0; i < NUM_LEDS; i++) {
            uint8_t desired = 0;
            if (currentState == FINISHED) {
                desired = ((millis() - timerStartTime) % 1000 < 500) ? 255 : 0;
            } else if (currentState == RUNNING) {
                desired = (i < litLeds) ? 255 : 0;
            }
            startFade(i, desired);
        }
    }

    // ── SMOOTH COLOR BLEND ────────────────────────────────────
    nblend(currentSettingColor, targetSettingColor, 25);

    // ── FADE EXECUTION & COLOR MAPPING ───────────────────────
    unsigned long now = millis();
    for (int i = 0; i < NUM_LEDS; i++) {
        if (currentBrightness[i] != targetBrightness[i]) {
            unsigned long elapsed = now - fadeStartTime[i];
            if (elapsed >= FADE_DURATION_MS) {
                currentBrightness[i] = targetBrightness[i];
            } else {
                float t     = (float)elapsed / (float)FADE_DURATION_MS;
                float eased = easeInOut(t);
                currentBrightness[i] = (uint8_t)(
                    fadeStartBrightness[i] + eased * ((int)targetBrightness[i] - (int)fadeStartBrightness[i])
                );
            }
        }

        if (currentState == SETTING || currentState == CONFIG_DEFAULT_TIME) {
            leds[i] = CRGB(
                (currentSettingColor.r * currentBrightness[i]) / 255,
                (currentSettingColor.g * currentBrightness[i]) / 255,
                (currentSettingColor.b * currentBrightness[i]) / 255
            );
        } else if (currentState == RUNNING) {
            leds[i] = CRGB(0, currentBrightness[i], 0);
        } else {
            leds[i] = CRGB(currentBrightness[i], 0, 0);
        }
    }

    FastLED.show();

    // ── OLED UPDATE (~10 FPS) ─────────────────────────────────
    static unsigned long lastPrint = 0;
    if (now - lastPrint >= 100) {
        display.clearDisplay();

        char timeBuf[16];

        if (currentState == SETTING || currentState == CONFIG_DEFAULT_TIME) {
            display.setFont(&FreeSansBold9pt7b);
            display.setCursor(0, 15); // Adafruit custom fonts draw from BASELINE
            if (currentState == CONFIG_DEFAULT_TIME) {
                display.print("Set Default:");
            } else {
                display.print("Set Timer:");
            }

            int h = val / 3600;
            int m = (val % 3600) / 60;
            int s = val % 60;

            display.setFont(&FreeSansBold12pt7b);
            sprintf(timeBuf, "%d:%02d:%02d", h, m, s);
            display.setCursor(12, 45);
            display.print(timeBuf);
        }
        else if (currentState == RUNNING) {
            display.setFont(&FreeSansBold9pt7b);
            display.setCursor(0, 15);
            display.print("Focusing:");

            unsigned long secsRemaining = timeRemainingMs / 1000;
            int h = secsRemaining / 3600;
            int m = (secsRemaining % 3600) / 60;
            int s = secsRemaining % 60;

            display.setFont(&FreeSansBold12pt7b);
            sprintf(timeBuf, "%d:%02d:%02d", h, m, s);
            display.setCursor(12, 45);
            display.print(timeBuf);

            // Progress bar — drains left-to-right as time elapses
            int barWidth = (int)map(timeRemainingMs, 0, timerDurationMs, 0, 126);
            barWidth = constrain(barWidth, 0, 126);
            display.drawRect(0, 56, 128, 7, SH110X_WHITE);  // Outline
            display.fillRect(1, 57, barWidth, 5, SH110X_WHITE); // Inner fill
        }
        else if (currentState == FINISHED) {
            display.setFont(&FreeSansBold12pt7b);
            display.setCursor(8, 35);
            display.print("TIME'S UP!");
        }

        display.display();
        lastPrint = now;
    }

    delay(10);
}