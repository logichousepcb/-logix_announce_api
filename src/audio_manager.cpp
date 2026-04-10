#include "audio_manager.h"
#include "../include/pins.h"
#include <Arduino.h>
#include <AudioFileSourceSD.h>
#include <AudioGenerator.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ─────────────────────────────────────────────
//  State
// ─────────────────────────────────────────────
static AudioOutputI2S*    output  = nullptr;
static AudioGenerator*    decoder = nullptr;
static AudioFileSourceSD* source  = nullptr;
static SemaphoreHandle_t  audio_mutex = nullptr;
static TaskHandle_t       audio_task_handle = nullptr;
static uint8_t            volume_percent = 50;
static volatile bool      playback_running = false;
static char               current_file_path[128] = "";

static bool hasExtensionIgnoreCase(const char* filename, const char* extension) {
    if (filename == nullptr || extension == nullptr) {
        return false;
    }

    size_t name_len = strlen(filename);
    size_t ext_len = strlen(extension);
    if (name_len < ext_len) {
        return false;
    }

    const char* end = filename + (name_len - ext_len);
    for (size_t i = 0; i < ext_len; ++i) {
        if (tolower(static_cast<unsigned char>(end[i])) !=
            tolower(static_cast<unsigned char>(extension[i]))) {
            return false;
        }
    }

    return true;
}

static AudioGenerator* createDecoderForPath(const char* path) {
    if (hasExtensionIgnoreCase(path, ".mp3")) {
        return new AudioGeneratorMP3();
    }

    if (hasExtensionIgnoreCase(path, ".wav")) {
        return new AudioGeneratorWAV();
    }

    return nullptr;
}

// ─────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────
static void closeSourceLocked() {
    if (source != nullptr) {
        delete source;
        source = nullptr;
    }
}

static void stopPlaybackLocked(bool log_stop) {
    if (decoder != nullptr && decoder->isRunning()) {
        decoder->stop();
        playback_running = false;
        current_file_path[0] = '\0';
        if (log_stop) {
            //Serial.println("I2S: Stopped");
        }
    }

    closeSourceLocked();

    if (decoder != nullptr) {
        delete decoder;
        decoder = nullptr;
    }
}

static void audioTask(void* parameter) {
    (void)parameter;

    while (true) {
        bool finished = false;

        if (audio_mutex != nullptr && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (decoder != nullptr && decoder->isRunning()) {
                if (!decoder->loop()) {
                    decoder->stop();
                    closeSourceLocked();
                    playback_running = false;
                    current_file_path[0] = '\0';
                    finished = true;
                }
            }
            xSemaphoreGive(audio_mutex);
        }

        if (finished) {
            //Serial.println("I2S: Playback finished");
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ─────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────
bool initAudio() {
    audio_mutex = xSemaphoreCreateMutex();
    if (audio_mutex == nullptr) {
        //Serial.println("I2S: Mutex create failed");
        return false;
    }

    output = new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S);
    // WT32-ETH01 uses GPIO0 as RMII clock input for LAN8720.
    // ESP32 I2S driver only accepts GPIO0/1/3 for MCLK, so avoid GPIO0
    // and route MCLK to GPIO3 to prevent ETH clock conflicts.
    output->SetMclk(true);
    output->SetPinout(PIN_I2S_BCK, PIN_I2S_LRCK, PIN_I2S_DOUT, 3);
    output->SetGain(static_cast<float>(volume_percent) / 100.0f);

    if (xTaskCreatePinnedToCore(audioTask,
                                "audioTask",
                                6144,
                                nullptr,
                                1,
                                &audio_task_handle,
                                1) != pdPASS) {
        //Serial.println("I2S: Audio task create failed");
        return false;
    }

    //Serial.println("I2S: Ready  (PCM5102A, ESP8266Audio)");
    return true;
}

bool audioPlayFile(const char* filename) {
    if (audio_mutex == nullptr || output == nullptr) {
        //Serial.println("I2S: Audio not initialised");
        return false;
    }

    // Build full SD path: prepend '/' if missing
    char path[128];
    if (filename[0] == '/') {
        snprintf(path, sizeof(path), "%s", filename);
    } else {
        snprintf(path, sizeof(path), "/%s", filename);
    }

    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        //Serial.println("I2S: Audio busy");
        return false;
    }

    stopPlaybackLocked(false);

    source = new AudioFileSourceSD(path);
    if (source == nullptr || !source->isOpen()) {
        //Serial.printf("I2S: File not found: %s\n", path);
        closeSourceLocked();
        xSemaphoreGive(audio_mutex);
        return false;
    }

    decoder = createDecoderForPath(path);
    if (decoder == nullptr) {
        closeSourceLocked();
        xSemaphoreGive(audio_mutex);
        return false;
    }

    if (!decoder->begin(source, output)) {
        //Serial.printf("I2S: Decoder failed for: %s\n", path);
        closeSourceLocked();
        delete decoder;
        decoder = nullptr;
        xSemaphoreGive(audio_mutex);
        return false;
    }

    strncpy(current_file_path, path, sizeof(current_file_path) - 1);
    current_file_path[sizeof(current_file_path) - 1] = '\0';
    playback_running = true;

    xSemaphoreGive(audio_mutex);

    //Serial.printf("I2S: Playing %s\n", path);
    return true;
}

const char* audioGetCurrentFile() {
    return current_file_path;
}

void audioStop() {
    if (audio_mutex == nullptr) {
        return;
    }

    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        stopPlaybackLocked(true);
        xSemaphoreGive(audio_mutex);
    }
}

bool audioIsPlaying() {
    if (audio_mutex == nullptr) {
        return false;
    }

    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        playback_running = decoder != nullptr && decoder->isRunning();
        xSemaphoreGive(audio_mutex);
        return playback_running;
    }

    // If the mutex is busy, return the last known state to avoid false idle transitions.
    return playback_running;
}

bool audioSetVolume(uint8_t new_volume_percent) {
    if (new_volume_percent > 100 || audio_mutex == nullptr || output == nullptr) {
        return false;
    }

    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }

    volume_percent = new_volume_percent;
    output->SetGain(static_cast<float>(volume_percent) / 100.0f);
    xSemaphoreGive(audio_mutex);

    //Serial.printf("I2S: Volume set to %u%%\n", volume_percent);
    return true;
}

uint8_t audioGetVolume() {
    uint8_t current = volume_percent;

    if (audio_mutex == nullptr) {
        return current;
    }

    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        current = volume_percent;
        xSemaphoreGive(audio_mutex);
    }

    return current;
}

void audioLoop() {
    // Playback runs in audioTask() so the main loop can service Ethernet and HTTP.
}
