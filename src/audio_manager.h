#pragma once
#include <stdint.h>

bool initAudio();
bool audioPlayFile(const char* filename);  // filename only, e.g. "beep.mp3" or "beep.wav"
bool audioPlayUrl(const char* url, bool buffered = false); // http:// or https:// URL. Use buffered=true for live streams.
void audioStop();
bool audioIsPlaying();
bool audioIsStreaming();                       // true when playing a URL stream
bool audioSetVolume(uint8_t volume_percent);  // 0..100
uint8_t audioGetVolume();
const char* audioGetCurrentFile();         // returns current filename, empty string if not playing
void audioLoop();                          // call from loop()
