#include "oled_manager.h"
#include "audio_manager.h"
#include "eth_manager.h"
#include "api_server.h"
#include "../include/pins.h"
#include <string.h>

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

namespace {
static const int OLED_WIDTH = 128;
static const int OLED_HEIGHT = 64;
static const uint8_t OLED_ADDR = 0x3C;
static const uint32_t OLED_REFRESH_MS = 500;

static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool oled_ready = false;
static uint32_t last_draw_ms = 0;
}

bool initOled() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        //Serial.println("OLED: Init failed");
        oled_ready = false;
        return false;
    }

    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("IP: Initializing...");
    oled.println("AUDIO: STOPPED");
    oled.println("VOL: 0%");
    oled.println("---");
    oled.display();

    oled_ready = true;
    //Serial.println("OLED: Ready");
    return true;
}

void oledLoop() {
    if (!oled_ready) {
        return;
    }

    uint32_t now = millis();
    if (now - last_draw_ms < OLED_REFRESH_MS) {
        return;
    }
    last_draw_ms = now;

    String ip = getNetworkIpAddress();
    if (ip.length() == 0 || ip == "0.0.0.0") {
        ip = "N/A";
    }

    const char* audio_state;
    if (audioIsPlaying()) {
        audio_state = "PLAYING";
    } else if (isQueueEnabled()) {
        audio_state = "QUEUED PLAY";
    } else {
        audio_state = "STOPPED";
    }
    uint8_t volume = audioGetVolume();

    // Current filename: strip leading '/' and truncate to 21 chars to fit 128px
    const char* raw_file = audioGetCurrentFile();
    if (raw_file[0] == '/') raw_file++;
    char file_label[22];
    strncpy(file_label, raw_file, sizeof(file_label) - 1);
    file_label[sizeof(file_label) - 1] = '\0';

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.print("IP: ");
    oled.println(ip);
    oled.print("AUDIO: ");
    oled.println(audio_state);
    oled.print("VOL: ");
    oled.print(volume);
    oled.println("%");
    oled.println(file_label[0] ? file_label : "---");
    oled.display();
}
