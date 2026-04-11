#include "audio_manager.h"
#include "../include/pins.h"
#include <Arduino.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioFileSourceBuffer.h>
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
static AudioOutputI2S*  output  = nullptr;
static AudioGenerator*  decoder = nullptr;
static AudioFileSource*       source         = nullptr;
static AudioFileSourceBuffer* http_buffer     = nullptr;
static SemaphoreHandle_t      audio_mutex     = nullptr;
static TaskHandle_t       audio_task_handle = nullptr;
static uint8_t            volume_percent = 50;
static volatile bool      playback_running = false;
static volatile bool      streaming_active = false;
static char               current_file_path[256] = "";
static uint32_t           stream_fail_since_ms = 0;

static const size_t URL_STREAM_BUFFER_BYTES = 32768;
static const uint32_t STREAM_UNDERRUN_GRACE_MS = 1500;

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

    // Many live stream URLs don't include a file extension.
    // Default to MP3, which is the most common internet radio format.
    return new AudioGeneratorMP3();

}

static void closeSourceLocked();

static bool startUrlPlaybackLocked(const char* url, bool buffered) {
    AudioFileSource* network_source = nullptr;

    if (buffered) {
        // Buffered mode is intended for long-running internet radio streams.
        network_source = new AudioFileSourceICYStream(url);
        if (network_source == nullptr || !network_source->isOpen()) {
            delete network_source;
            network_source = new AudioFileSourceHTTPStream(url);
        }
    } else {
        // For finite HTTP media (TTS/proxy files), HTTP/1.0 helps guarantee EOF.
        AudioFileSourceHTTPStream* http_source = new AudioFileSourceHTTPStream();
        if (http_source != nullptr) {
            http_source->useHTTP10();
            if (http_source->open(url)) {
                network_source = http_source;
            } else {
                delete http_source;
            }
        }
    }

    if (network_source == nullptr || !network_source->isOpen()) {
        delete network_source;
        return false;
    }

    source = network_source;

    decoder = createDecoderForPath(url);
    if (decoder == nullptr) {
        closeSourceLocked();
        return false;
    }

    AudioFileSource* decode_source = network_source;
    if (buffered) {
        // Use a large pre-fill buffer for smooth live-stream playback.
        http_buffer = new AudioFileSourceBuffer(network_source, URL_STREAM_BUFFER_BYTES);
        if (http_buffer == nullptr) {
            closeSourceLocked();
            delete decoder;
            decoder = nullptr;
            return false;
        }
        decode_source = http_buffer;
    }

    if (!decoder->begin(decode_source, output)) {
        closeSourceLocked();
        delete decoder;
        decoder = nullptr;
        return false;
    }

    strncpy(current_file_path, url, sizeof(current_file_path) - 1);
    current_file_path[sizeof(current_file_path) - 1] = '\0';
    playback_running = true;
    streaming_active = true;
    stream_fail_since_ms = 0;
    return true;
}

// ─────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────
static void closeSourceLocked() {
    if (http_buffer != nullptr) {
        delete http_buffer;
        http_buffer = nullptr;
    }
    if (source != nullptr) {
        delete source;
        source = nullptr;
    }
}

static void stopPlaybackLocked(bool log_stop) {
    if (decoder != nullptr && decoder->isRunning()) {
        decoder->stop();
        playback_running = false;
        streaming_active = false;
        stream_fail_since_ms = 0;
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
            if (decoder != nullptr) {
                if (decoder->isRunning()) {
                    if (decoder->loop()) {
                        stream_fail_since_ms = 0;
                    } else {
                        if (streaming_active) {
                            uint32_t now = millis();
                            if (stream_fail_since_ms == 0) {
                                stream_fail_since_ms = now;
                            }

                            // Brief network jitter is common for internet streams.
                            // Keep decoder alive for a short grace window before stopping.
                            if (now - stream_fail_since_ms < STREAM_UNDERRUN_GRACE_MS) {
                                xSemaphoreGive(audio_mutex);
                                vTaskDelay(pdMS_TO_TICKS(2));
                                continue;
                            }
                        }

                        decoder->stop();
                        closeSourceLocked();
                        delete decoder;
                        decoder = nullptr;
                        playback_running = false;
                        streaming_active = false;
                        stream_fail_since_ms = 0;
                        current_file_path[0] = '\0';
                        finished = true;
                    }
                } else if (playback_running) {
                    closeSourceLocked();
                    delete decoder;
                    decoder = nullptr;
                    playback_running = false;
                    streaming_active = false;
                    stream_fail_since_ms = 0;
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
                                2,
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
    streaming_active = false;

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

bool audioPlayUrl(const char* url, bool buffered) {
    if (audio_mutex == nullptr || output == nullptr) {
        return false;
    }

    if (url == nullptr ||
        (strncmp(url, "http://",  7) != 0 &&
         strncmp(url, "https://", 8) != 0)) {
        return false;
    }

    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }

    stopPlaybackLocked(false);

    bool started = startUrlPlaybackLocked(url, buffered);

    xSemaphoreGive(audio_mutex);
    return started;
}

bool audioIsStreaming() {
    if (audio_mutex == nullptr) {
        return false;
    }

    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool result = streaming_active && decoder != nullptr && decoder->isRunning();
        xSemaphoreGive(audio_mutex);
        return result;
    }

    return streaming_active;
}

void audioLoop() {
    // Playback runs in audioTask() so the main loop can service Ethernet and HTTP.
}
