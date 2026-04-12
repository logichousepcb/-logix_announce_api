#include <Arduino.h>

#include "api_server.h"
#include "audio_manager.h"
#include "eth_manager.h"
#include "oled_manager.h"
#include "sd_manager.h"
#include "../include/pins.h"

// Factory reset: hold GPIO2 (PIN_BUTTON_2) to GND for 10 seconds
static const uint32_t FACTORY_RESET_HOLD_MS = 10000;
static uint32_t factory_reset_hold_start_ms = 0;

void setup() {
    pinMode(PIN_BUTTON_2, INPUT_PULLUP);
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("ESP BOOTED!");
    Serial.println("BOOT: init SD");

    bool sd_ready = initSD();
    if (!sd_ready) {
        Serial.println("SD init failed");
    } else {
        Serial.println("BOOT: SD ready");
    }

    Serial.println("BOOT: init audio");
    bool audio_ready = initAudio();
    if (!audio_ready) {
        Serial.println("Audio init failed");
    } else {
        Serial.println("BOOT: audio ready");
    }

    Serial.println("BOOT: init OLED");
    bool oled_ready = initOled();
    if (!oled_ready) {
        Serial.println("OLED init failed");
    } else {
        Serial.println("BOOT: OLED ready");
    }

    Serial.println("BOOT: init network");
    initNetwork();
    Serial.println("BOOT: network init returned");

    if (sd_ready) {
        Serial.println("BOOT: init API server");
        initApiServer();
        Serial.println("BOOT: API server ready");
    } else {
        Serial.println("API server skipped because SD init failed");
    }

    Serial.println("BOOT: setup complete");
}

void loop() {
    handleApiServer();
    audioLoop();
    oledLoop();

    // Factory reset detection: GPIO2 held LOW for 10 seconds
    if (digitalRead(PIN_BUTTON_2) == LOW) {
        if (factory_reset_hold_start_ms == 0) {
            factory_reset_hold_start_ms = millis();
        } else if (millis() - factory_reset_hold_start_ms >= FACTORY_RESET_HOLD_MS) {
            factory_reset_hold_start_ms = 0;
            oledShowFactoryReset();
            factoryResetCredentials();
            delay(3000);
            ESP.restart();
        }
    } else {
        factory_reset_hold_start_ms = 0;
    }

    delay(1);
}
