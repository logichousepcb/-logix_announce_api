#include <Arduino.h>

#include "api_server.h"
#include "audio_manager.h"
#include "eth_manager.h"
#include "oled_manager.h"
#include "sd_manager.h"

void setup() {
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
    delay(1);
}
