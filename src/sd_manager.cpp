#include "sd_manager.h"
#include "../include/pins.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// ─────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────
static int countFiles(File dir) {
    int count = 0;
    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        if (!entry.isDirectory()) count++;
        entry.close();
    }
    return count;
}

static void printFiles(File dir) {
    dir.rewindDirectory();
    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        if (!entry.isDirectory()) {
            //Serial.print("  ");
            //Serial.print(entry.name());
            //Serial.print("  (");
            //Serial.print(entry.size());
            //Serial.println(" bytes)");
        }
        entry.close();
    }
}

// ─────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────
bool initSD() {
    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

    if (!SD.begin(PIN_SD_CS, SPI, SD_SPI_HZ)) {
        //Serial.println("SD:  Mount failed");
        return false;
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    //Serial.print("SD:  Mounted  (");
    //Serial.print(cardSize);
    //Serial.println(" MB)");
    return true;
}

void listSDFiles() {
    File root = SD.open("/");
    if (!root) {
        //Serial.println("SD:  Failed to open root");
        return;
    }

    //Serial.println("SD:  Files on card:");
    printFiles(root);

    root.rewindDirectory();
    int total = countFiles(root);
    root.close();

    //Serial.print("SD:  Total files: ");
    //Serial.println(total);
}
