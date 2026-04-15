#include "api_server.h"
#include "audio_manager.h"
#include "eth_manager.h"
#include "../include/build_version.h"
#include <Arduino.h>
#include <Update.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD.h>
#include <ctype.h>

// ─────────────────────────────────────────────
//  Server instance
// ─────────────────────────────────────────────
static WebServer server(80);
static bool server_running = false;
static File upload_file;
static bool upload_failed = false;
static int upload_status_code = 200;
static String upload_error;
static String upload_saved_name;
static bool firmware_upload_failed = false;
static int firmware_upload_status_code = 200;
static String firmware_upload_error;
static String firmware_uploaded_version;
static bool firmware_reboot_pending = false;
static uint32_t firmware_reboot_at_ms = 0;
static const char* playlist_state_path = "/playlist_state.json";
static const char* queue_playlist_path = "/playlist.m3u";
static String active_playlist_files[64];
static size_t active_playlist_count = 0;
static bool queued_play_enabled = false;
static size_t queued_play_index = 0;
static uint32_t next_queue_attempt_ms = 0;
static uint32_t queue_idle_since_ms = 0;
static uint16_t queue_pause_seconds = 1;
static bool url_loop_enabled = false;
static bool url_loop_armed = false;
static String last_stream_url = "";
static uint32_t url_loop_idle_since_ms = 0;
static uint32_t next_url_loop_attempt_ms = 0;
static String last_network_mode = "eth";
static bool last_network_connected = false;
static String last_network_ip = "0.0.0.0";
static String last_network_mac = "";
static String device_username = "logix";
static String device_password = "logix";
static bool webui_auth_enabled = true;
static const char* prefs_namespace = "logix_api";
static const char* prefs_user_key = "ui_user";
static const char* prefs_pass_key = "ui_pass";
static const char* prefs_auth_enabled_key = "ui_auth_en";

// ─────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────
static void sendJson(int code, const char* status, const char* message) {
    JsonDocument doc;
    doc["status"]  = status;
    doc["message"] = message;
    String body;
    serializeJson(doc, body);
    server.send(code, "application/json", body);
}

static void sendJsonDocument(int code, JsonDocument& doc) {
    String body;
    serializeJson(doc, body);
    server.send(code, "application/json", body);
}

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

static bool hasSupportedAudioExtension(const char* filename) {
    return hasExtensionIgnoreCase(filename, ".mp3") ||
           hasExtensionIgnoreCase(filename, ".wav");
}

static bool hasPlaylistExtension(const char* filename) {
    return hasExtensionIgnoreCase(filename, ".m3u") ||
           hasExtensionIgnoreCase(filename, ".m3u8");
}

static bool hasSupportedLibraryFileExtension(const char* filename) {
    return hasSupportedAudioExtension(filename) || hasPlaylistExtension(filename);
}

static bool hasFirmwareExtension(const char* filename) {
    return hasExtensionIgnoreCase(filename, ".bin");
}

static const char* getAudioStateLabel() {
    if (audioIsPlaying()) {
        return "PLAYING";
    }

    if (queued_play_enabled) {
        return "QUEUED PLAY";
    }

    return "STOPPED";
}

static void buildAudioFileLabel(char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    const char* raw_file = audioGetCurrentFile();
    if (raw_file == nullptr) {
        raw_file = "";
    }

    if (raw_file[0] == '/') {
        raw_file++;
    }

    strncpy(out, raw_file, out_size - 1);
    out[out_size - 1] = '\0';
}

static bool normalizeFilename(const char* input, char* out, size_t out_size) {
    if (input == nullptr || out == nullptr || out_size < 3) {
        return false;
    }

    while (*input == '/') {
        input++;
    }

    if (*input == '\0' || strstr(input, "..") != nullptr) {
        return false;
    }

    if (strchr(input, '/') != nullptr || strchr(input, '\\') != nullptr) {
        return false;
    }

    size_t name_len = strlen(input);
    if (name_len + 2 >= out_size) {
        return false;
    }

    out[0] = '/';
    memcpy(out + 1, input, name_len + 1);
    return true;
}

static int findActivePlaylistIndex(const String& filename) {
    for (size_t i = 0; i < active_playlist_count; ++i) {
        if (active_playlist_files[i].equals(filename)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static bool isFileActiveInPlaylist(const String& filename) {
    return findActivePlaylistIndex(filename) >= 0;
}

// Forward declaration because state loading happens before the parser definition.
static size_t parseM3uTracks(const char* playlist_path, String* out_tracks, size_t max_tracks);

static void updateLastNetworkSnapshot() {
    last_network_mode = getNetworkModeString();
    last_network_connected = isNetworkConnected();
    last_network_ip = getNetworkIpAddress();
    last_network_mac = getNetworkMacAddress();
}

static void normalizeDeviceUsername(String& username) {
    username.trim();
    if (username.length() == 0) {
        username = "logix";
    }
    if (username.length() > 8) {
        username = username.substring(0, 8);
    }
}

static bool loadDeviceCredentialsFromPrefs() {
    Preferences prefs;
    if (!prefs.begin(prefs_namespace, true)) {
        device_username = "logix";
        device_password = "logix";
        webui_auth_enabled = true;
        return false;
    }

    String loaded_user = prefs.getString(prefs_user_key, "logix");
    String loaded_pass = prefs.getString(prefs_pass_key, "logix");
    bool loaded_auth_enabled = prefs.getBool(prefs_auth_enabled_key, true);
    prefs.end();

    normalizeDeviceUsername(loaded_user);
    if (loaded_pass.length() == 0) {
        loaded_pass = "logix";
    }

    device_username = loaded_user;
    device_password = loaded_pass;
    webui_auth_enabled = loaded_auth_enabled;
    return true;
}

static bool saveDeviceCredentialsToPrefs() {
    Preferences prefs;
    if (!prefs.begin(prefs_namespace, false)) {
        return false;
    }

    prefs.putString(prefs_user_key, device_username);
    prefs.putString(prefs_pass_key, device_password);
    prefs.putBool(prefs_auth_enabled_key, webui_auth_enabled);
    prefs.end();
    return true;
}

static bool savePlaylistState() {
    updateLastNetworkSnapshot();

    if (SD.exists(playlist_state_path)) {
        SD.remove(playlist_state_path);
    }

    File file = SD.open(playlist_state_path, FILE_WRITE);
    if (!file) {
        return false;
    }

    JsonDocument doc;
    doc["queue_enabled"] = queued_play_enabled;
    doc["queue_pause_seconds"] = queue_pause_seconds;
    doc["url_loop_enabled"] = url_loop_enabled;
    doc["wifi_ssid"] = getWifiSsid();
    doc["wifi_password"] = getWifiPassword();
    doc["network_mode"] = (getNetworkMode() == NETWORK_MODE_WIFI) ? "wifi" : "eth";
    doc["last_network_mode"] = last_network_mode;
    doc["last_network_connected"] = last_network_connected;
    doc["last_network_ip"] = last_network_ip;
    doc["last_network_mac"] = last_network_mac;

    bool ok = serializeJson(doc, file) > 0;
    file.close();

    if (!ok) {
        return false;
    }

    if (SD.exists(queue_playlist_path)) {
        SD.remove(queue_playlist_path);
    }

    File queue_file = SD.open(queue_playlist_path, FILE_WRITE);
    if (!queue_file) {
        return false;
    }

    queue_file.println("#EXTM3U");
    queue_file.println("# Auto-generated queued file list");
    for (size_t i = 0; i < active_playlist_count; ++i) {
        queue_file.println(active_playlist_files[i]);
    }
    queue_file.close();

    return true;
}

static void loadPlaylistState() {
    active_playlist_count = 0;
    queued_play_enabled = false;
    queued_play_index = 0;
    queue_pause_seconds = 1;
    url_loop_enabled = false;
    url_loop_armed = false;
    last_stream_url = "";
    url_loop_idle_since_ms = 0;
    next_url_loop_attempt_ms = 0;

    if (SD.exists(playlist_state_path)) {
        File file = SD.open(playlist_state_path, FILE_READ);
        if (file) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, file);
            file.close();
            if (!err) {
                queued_play_enabled = doc["queue_enabled"] | false;
                uint16_t loaded_pause = doc["queue_pause_seconds"] | 1;
                if (loaded_pause < 1) {
                    loaded_pause = 1;
                } else if (loaded_pause > 180) {
                    loaded_pause = 180;
                }
                queue_pause_seconds = loaded_pause;
                url_loop_enabled = doc["url_loop_enabled"] | false;

                if (doc["wifi_ssid"].is<const char*>() || doc["wifi_password"].is<const char*>()) {
                    String saved_ssid = doc["wifi_ssid"] | "";
                    String saved_password = doc["wifi_password"] | "";
                    setWifiCredentials(saved_ssid, saved_password);
                }

                if (doc["network_mode"].is<const char*>()) {
                    String mode_str = doc["network_mode"] | "eth";
                    setNetworkMode(mode_str == "wifi" ? NETWORK_MODE_WIFI : NETWORK_MODE_ETH);
                }

                if (doc["last_network_mode"].is<const char*>()) {
                    last_network_mode = doc["last_network_mode"].as<const char*>();
                }
                last_network_connected = doc["last_network_connected"] | false;
                if (doc["last_network_ip"].is<const char*>()) {
                    last_network_ip = doc["last_network_ip"].as<const char*>();
                }
                if (doc["last_network_mac"].is<const char*>()) {
                    last_network_mac = doc["last_network_mac"].as<const char*>();
                }
            }
        }
    }

    String tracks[64];
    size_t count = parseM3uTracks(queue_playlist_path, tracks, 64);
    for (size_t i = 0; i < count; ++i) {
        active_playlist_files[active_playlist_count++] = tracks[i];
    }

    if (active_playlist_count == 0) {
        queued_play_enabled = false;
    }
}

static bool setFileActiveInPlaylist(const String& filename, bool active) {
    int idx = findActivePlaylistIndex(filename);

    if (active) {
        if (idx >= 0) {
            return true;
        }
        if (active_playlist_count >= (sizeof(active_playlist_files) / sizeof(active_playlist_files[0]))) {
            return false;
        }
        active_playlist_files[active_playlist_count++] = filename;
        return true;
    }

    if (idx < 0) {
        return true;
    }

    for (size_t i = static_cast<size_t>(idx); i + 1 < active_playlist_count; ++i) {
        active_playlist_files[i] = active_playlist_files[i + 1];
    }
    if (active_playlist_count > 0) {
        active_playlist_files[active_playlist_count - 1] = "";
        active_playlist_count--;
    }

    if (active_playlist_count == 0) {
        queued_play_enabled = false;
        queued_play_index = 0;
    } else if (queued_play_index >= active_playlist_count) {
        queued_play_index = 0;
    }

    return true;
}

// Parse tracks from an M3U/M3U8 file into out_tracks[]. Returns count (0 on failure).
static size_t parseM3uTracks(const char* playlist_path, String* out_tracks, size_t max_tracks) {
    if (playlist_path == nullptr || max_tracks == 0) {
        return 0;
    }

    File playlist = SD.open(playlist_path, FILE_READ);
    if (!playlist) {
        return 0;
    }

    size_t count = 0;

    while (playlist.available() && count < max_tracks) {
        String line = playlist.readStringUntil('\n');
        line.trim();

        if (line.length() == 0 || line.startsWith("#")) {
            continue;
        }

        if (line.startsWith("http://") || line.startsWith("https://")) {
            continue;
        }

        char path[128];
        if (!normalizeFilename(line.c_str(), path, sizeof(path))) {
            continue;
        }

        if (!hasSupportedAudioExtension(path) || !SD.exists(path)) {
            continue;
        }

        String normalized = String(path + 1);
        bool duplicate = false;
        for (size_t i = 0; i < count; ++i) {
            if (out_tracks[i].equals(normalized)) {
                duplicate = true;
                break;
            }
        }

        if (!duplicate) {
            out_tracks[count++] = normalized;
        }
    }

    playlist.close();
    return count;
}

static bool loadQueueFromM3uFile(const char* playlist_path, String* error_message) {
    if (playlist_path == nullptr || !hasPlaylistExtension(playlist_path)) {
        if (error_message != nullptr) {
            *error_message = "Playlist must be .m3u or .m3u8";
        }
        return false;
    }

    if (!SD.exists(playlist_path)) {
        if (error_message != nullptr) {
            *error_message = "Failed to open playlist";
        }
        return false;
    }

    String tracks[64];
    size_t count = parseM3uTracks(playlist_path, tracks, 64);

    if (count == 0) {
        if (error_message != nullptr) {
            *error_message = "Playlist has no playable entries";
        }
        return false;
    }

    active_playlist_count = 0;
    for (size_t i = 0; i < count; ++i) {
        active_playlist_files[active_playlist_count++] = tracks[i];
    }

    queued_play_enabled = true;
    queued_play_index = 0;
    next_queue_attempt_ms = 0;
    queue_idle_since_ms = millis() - 500;
    return true;
}

static void serviceQueuedPlayback() {
    if (!queued_play_enabled || active_playlist_count == 0) {
        queue_idle_since_ms = 0;
        return;
    }

    uint32_t now = millis();

    if (audioIsPlaying()) {
        queue_idle_since_ms = 0;
        return;
    }

    // Require a stable idle window before starting the next track.
    if (queue_idle_since_ms == 0) {
        queue_idle_since_ms = now;
        return;
    }

    uint32_t required_idle_ms = static_cast<uint32_t>(queue_pause_seconds) * 1000U;
    if (required_idle_ms < 500U) {
        required_idle_ms = 500U;
    }

    if (now - queue_idle_since_ms < required_idle_ms) {
        return;
    }

    if (now < next_queue_attempt_ms) {
        return;
    }

    for (size_t attempt = 0; attempt < active_playlist_count; ++attempt) {
        if (queued_play_index >= active_playlist_count) {
            queued_play_index = 0;
        }

        String filename = active_playlist_files[queued_play_index];
        queued_play_index = (queued_play_index + 1) % active_playlist_count;

        char path[128];
        if (!normalizeFilename(filename.c_str(), path, sizeof(path)) || !SD.exists(path)) {
            continue;
        }

        if (audioPlayFile(filename.c_str())) {
            queue_idle_since_ms = 0;
            next_queue_attempt_ms = now + 250;
            return;
        }
    }

    next_queue_attempt_ms = now + 1500;
}

static void serviceUrlLoopPlayback() {
    if (!url_loop_armed || last_stream_url.length() == 0) {
        url_loop_idle_since_ms = 0;
        return;
    }

    if (queued_play_enabled) {
        url_loop_idle_since_ms = 0;
        return;
    }

    uint32_t now = millis();

    if (audioIsPlaying()) {
        url_loop_idle_since_ms = 0;
        return;
    }

    if (url_loop_idle_since_ms == 0) {
        url_loop_idle_since_ms = now;
        return;
    }

    uint32_t required_idle_ms = static_cast<uint32_t>(queue_pause_seconds) * 1000U;
    if (required_idle_ms < 500U) {
        required_idle_ms = 500U;
    }

    if (now - url_loop_idle_since_ms < required_idle_ms) {
        return;
    }

    if (now < next_url_loop_attempt_ms) {
        return;
    }

    if (audioPlayUrl(last_stream_url.c_str(), url_loop_enabled)) {
        url_loop_idle_since_ms = 0;
        next_url_loop_attempt_ms = now + 250;
        return;
    }

    next_url_loop_attempt_ms = now + 1500;
}

static String formatBytes(uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        return String(buf);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    return String(buf);
}

static void handleWebUi() {
    if (webui_auth_enabled && !server.authenticate(device_username.c_str(), device_password.c_str())) {
        server.requestAuthentication(BASIC_AUTH, "Logix Announcer");
        return;
    }

    String mac = getNetworkMacAddress();
    String ip = getNetworkIpAddress();
    String mode = getNetworkModeString();

    if (mac.length() == 0) {
        mac = "Unavailable";
    }
    if (ip.length() == 0 || ip == "0.0.0.0") {
        ip = "Unavailable";
    }

    uint64_t sd_total = SD.totalBytes();
    uint64_t sd_used  = SD.usedBytes();
    uint64_t sd_free  = (sd_total > sd_used) ? (sd_total - sd_used) : 0;
    String sd_used_str = formatBytes(sd_used);
    String sd_free_str = formatBytes(sd_free);

    String version = BUILD_VERSION;

        String html = R"HTML(
<!doctype html>
<html>
<head>
    <meta charset='utf-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1'>
    <title>Logix Message Announcer</title>
    <style>
        body { margin: 0; font-family: Segoe UI, Arial, sans-serif; background: linear-gradient(135deg, #eef4ff, #f7fbff); color: #0f172a; }
        .wrap { min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 24px; }
        .card { width: min(900px, 100%); background: #fff; border: 1px solid #dbe6ff; border-radius: 16px; padding: 24px; box-shadow: 0 10px 30px rgba(8, 35, 94, .08); }
        h1 { margin: 0 0 18px; font-size: clamp(24px, 4vw, 36px); letter-spacing: .2px; }
        .version { margin: 0 0 4px; font-size: 12px; color: #64748b; letter-spacing: .08em; text-transform: uppercase; }
        h2 { margin: 24px 0 12px; font-size: 22px; }
        .row { display: flex; flex-wrap: wrap; gap: 12px; }
        .item { flex: 1 1 260px; background: #f8fbff; border: 1px solid #d7e8ff; border-radius: 12px; padding: 12px 14px; }
        .label { display: block; font-size: 12px; color: #4b5563; text-transform: uppercase; letter-spacing: .08em; margin-bottom: 4px; }
        .value { font-size: 18px; font-weight: 600; color: #0b3a7e; word-break: break-word; }
        .split { display: flex; gap: 14px; }
        .split-col { flex: 1 1 0; min-width: 110px; }
        .tools { margin-top: 8px; display: flex; flex-wrap: wrap; gap: 10px; align-items: center; }
        .status { margin-top: 10px; min-height: 22px; color: #334155; font-size: 14px; }
        .table { width: 100%; border-collapse: collapse; margin-top: 12px; }
        .table th, .table td { border-bottom: 1px solid #e5edff; padding: 10px 8px; text-align: left; font-size: 14px; }
        .table th { color: #475569; font-weight: 600; }
        .btn { border: 0; border-radius: 9px; padding: 8px 12px; font-size: 13px; font-weight: 600; cursor: pointer; }
        .btn-play { background: #dbeafe; color: #1d4ed8; }
        .btn-del { background: #fee2e2; color: #b91c1c; margin-left: 8px; }
        .btn-upload { background: #dcfce7; color: #166534; }
        .pause-label { font-size: 13px; color: #334155; }
        .pause-input { width: 70px; padding: 7px 8px; border: 1px solid #cbd5e1; border-radius: 8px; font-size: 13px; }
        .net-input { width: 180px; padding: 7px 8px; border: 1px solid #cbd5e1; border-radius: 8px; font-size: 13px; }
        .version-status { margin-left: 6px; color: #475569; }
        .version-status.up-to-date { color: #15803d; font-weight: 700; }
        .version-link { cursor: pointer; text-decoration: underline dotted; }
        .modal-backdrop { position: fixed; inset: 0; background: rgba(15, 23, 42, 0.55); display: none; align-items: center; justify-content: center; padding: 18px; z-index: 20; }
        .modal-backdrop.open { display: flex; }
        .modal { width: min(100%, 460px); background: #ffffff; border-radius: 16px; box-shadow: 0 30px 70px rgba(15, 23, 42, 0.28); padding: 20px; }
        .modal h3 { margin: 0 0 10px; color: #0f172a; }
        .modal p { margin: 0 0 12px; color: #475569; font-size: 14px; line-height: 1.4; }
        .modal-actions { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 14px; }
        .btn-secondary { background: #e2e8f0; color: #1e293b; }
        .firmware-input { width: 100%; font-size: 14px; }
        input[type='file'] { font-size: 14px; }
    </style>
</head>
<body>
    <main class='wrap'>
        <section class='card'>
            <h1>Logix Message Announcer</h1>
            <p class='version'>Version <span id='firmwareVersion' class='version-link' title='Download newest firmware binary'>__VERSION__</span><span id='versionStatus' class='version-status'></span></p>
            <div class='row'>
                <div class='item'><span class='label'>MAC Address</span><span id='macAddress' class='value'>__MAC__</span></div>
                <div class='item'><span class='label'>IP Address</span><span id='ipAddress' class='value'>__IP__</span></div>
                <div class='item'>
                    <span class='label'>SD Storage</span>
                    <div class='split'>
                        <div class='split-col'><span class='label'>Used</span><span class='value'>__SD_USED__</span></div>
                        <div class='split-col'><span class='label'>Available</span><span class='value'>__SD_FREE__</span></div>
                    </div>
                </div>
                <div class='item'>
                    <span class='label'>Network</span>
                    <div class='tools' style='margin-top:4px'>
                        <select id='networkMode'>
                            <option value='eth'>ETH</option>
                            <option value='wifi'>WIFI</option>
                        </select>
                        <button class='btn' onclick='applyNetworkMode()'>Apply</button>
                    </div>
                    <div class='tools' style='margin-top:8px'>
                        <input id='wifiSsid' class='net-input' type='text' placeholder='WiFi Username (SSID)'>
                        <input id='wifiPassword' class='net-input' type='password' placeholder='WiFi Password'>
                        <button class='btn' onclick='saveWifiConfig()'>Save WiFi</button>
                    </div>
                    <div class='tools' style='margin-top:8px'>
                        <input id='deviceUsername' class='net-input' type='text' maxlength='8' placeholder='User (max 8)'>
                        <input id='devicePassword' class='net-input' type='password' placeholder='Password'>
                        <label class='pause-label'><input id='webUiAuthEnabled' type='checkbox'> Require Web UI Login</label>
                        <button class='btn' onclick='saveDeviceCredentials()'>Save User/Pass</button>
                    </div>
                </div>
            </div>

            <h2>File Manager</h2>
            <p style='margin: 0 0 12px; font-size: 13px; color: #64748b;'><strong>Note:</strong> For optimal playback performance, use uncompressed WAV audio. MP3 decoding increases CPU load and may impact buffering and network performance on the ESP32 platform.</p>
            <div class='tools'>
                <input id='audioFile' type='file' accept='.mp3,.wav,.m3u,.m3u8,audio/mpeg,audio/wav,audio/x-wav,audio/x-mpegurl,application/vnd.apple.mpegurl,text/plain'>
                <button class='btn btn-upload' onclick='uploadFile()'>Upload File</button>
                <button class='btn' onclick='loadFiles()'>Refresh</button>
                <button id='queueToggle' class='btn btn-play' onclick='toggleQueue()'>Play Queued: OFF</button>
                <span class='pause-label'>Pause (sec)</span>
                <input id='queuePauseSeconds' class='pause-input' type='number' min='1' max='180' value='1'>
                <button class='btn' onclick='saveQueuePause()'>Save Pause</button>
            </div>
            <div class='tools' style='margin-top:10px'>
                <input id='streamUrl' class='net-input' type='text' placeholder='http://example.com/stream.mp3' style='width: 320px;'>
                <label class='pause-label'><input id='urlLoopEnabled' type='checkbox'> Loop URL</label>
                <button class='btn btn-play' onclick='playUrl()'>Play URL</button>
                <button class='btn btn-del' onclick='stopPlayback()'>Stop Playback</button>
            </div>
            <div class='tools' style='margin-top:10px; align-items:center;'>
                <span class='pause-label'>Volume</span>
                <input id='volumeSlider' type='range' min='0' max='100' value='50' style='width:180px; cursor:pointer;' oninput='document.getElementById("volumeLabel").textContent=this.value' onchange='setVolume(this.value)'>
                <span id='volumeLabel' style='min-width:30px; text-align:right;'>50</span><span style='margin-left:2px;'>%</span>
            </div>
            <div id='status' class='status'>Loading files...</div>

            <table class='table'>
                <thead><tr><th>Active</th><th>File</th><th>Size (bytes)</th><th>Actions</th></tr></thead>
                <tbody id='fileRows'></tbody>
            </table>
        </section>
    </main>

    <div id='firmwareModal' class='modal-backdrop' onclick='dismissFirmwareModal(event)'>
        <div class='modal'>
            <h3>Firmware Update</h3>
            <p id='firmwareModalSummary'>Select how you want to update the device firmware.</p>
            <input id='firmwareFile' class='firmware-input' type='file' accept='.bin,application/octet-stream'>
            <div class='modal-actions'>
                <button id='updateLatestButton' class='btn btn-upload' onclick='updateToLatestFirmware()'>Update To Latest</button>
                <button class='btn' onclick='uploadSelectedFirmware()'>Upload Selected .bin</button>
                <button class='btn btn-secondary' onclick='closeFirmwareModal()'>Close</button>
            </div>
        </div>
    </div>

    <script>
        const repoTagsUrl = 'https://api.github.com/repos/logichousepcb/-logix_announce_api/tags?per_page=20';
        const repoRawBaseUrl = 'https://raw.githubusercontent.com/logichousepcb/-logix_announce_api/';
        let latestRepoVersion = null;

        function setStatus(msg) {
            document.getElementById('status').textContent = msg;
        }

        function buildReleaseBinaryUrl(versionText) {
            const normalizedVersion = normalizeVersion(versionText);
            if (!normalizedVersion) {
                return null;
            }
            return repoRawBaseUrl + 'v' + normalizedVersion + '/releases/logix_announce_api-' + normalizedVersion + '.bin';
        }

        function handleVersionClick() {
            const versionNode = document.getElementById('firmwareVersion');
            const modal = document.getElementById('firmwareModal');
            const summaryNode = document.getElementById('firmwareModalSummary');
            const latestButton = document.getElementById('updateLatestButton');
            if (!versionNode || !modal || !summaryNode || !latestButton) {
                return;
            }

            const currentVersion = normalizeVersion(versionNode.textContent);
            const targetVersion = latestRepoVersion || currentVersion;
            if (latestRepoVersion && compareVersions(latestRepoVersion, currentVersion) > 0) {
                summaryNode.textContent = 'Current version ' + currentVersion + '. Latest repo version is ' + latestRepoVersion + '.';
            } else {
                summaryNode.textContent = 'Current version ' + currentVersion + ' is up to date. You can reinstall it or upload another .bin file.';
            }
            latestButton.textContent = 'Update To ' + targetVersion;
            modal.classList.add('open');
        }

        function closeFirmwareModal() {
            const modal = document.getElementById('firmwareModal');
            if (modal) {
                modal.classList.remove('open');
            }
        }

        function dismissFirmwareModal(event) {
            if (event.target && event.target.id === 'firmwareModal') {
                closeFirmwareModal();
            }
        }

        async function postFirmwareBlob(blob, filename) {
            const formData = new FormData();
            formData.append('file', blob, filename);

            const res = await fetch('/firmware/update', {
                method: 'POST',
                body: formData
            });

            const data = await res.json();
            if (!res.ok || data.status !== 'ok') {
                throw new Error(data.message || 'Firmware update failed');
            }

            return data;
        }

        async function updateToLatestFirmware() {
            const versionNode = document.getElementById('firmwareVersion');
            const currentVersion = normalizeVersion(versionNode ? versionNode.textContent : '');
            const targetVersion = latestRepoVersion || currentVersion;
            const releaseUrl = buildReleaseBinaryUrl(targetVersion);
            if (!releaseUrl) {
                setStatus('Error: No firmware release URL available.');
                return;
            }

            try {
                setStatus('Downloading firmware ' + targetVersion + '...');
                const firmwareRes = await fetch(releaseUrl);
                if (!firmwareRes.ok) {
                    throw new Error('Failed to download firmware ' + targetVersion);
                }

                const firmwareBlob = await firmwareRes.blob();
                setStatus('Uploading firmware ' + targetVersion + '...');
                const data = await postFirmwareBlob(firmwareBlob, 'logix_announce_api-' + targetVersion + '.bin');
                closeFirmwareModal();
                setStatus(data.message || 'Firmware uploaded. Device will reboot shortly.');
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function uploadSelectedFirmware() {
            const input = document.getElementById('firmwareFile');
            if (!input || !input.files || input.files.length === 0) {
                setStatus('Select a .bin firmware file first.');
                return;
            }

            const firmwareFile = input.files[0];
            try {
                setStatus('Uploading firmware ' + firmwareFile.name + '...');
                const data = await postFirmwareBlob(firmwareFile, firmwareFile.name);
                closeFirmwareModal();
                input.value = '';
                setStatus(data.message || 'Firmware uploaded. Device will reboot shortly.');
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        function parseVersionParts(versionText) {
            const clean = String(versionText || '').trim().replace(/^v/i, '');
            if (!clean) {
                return null;
            }

            const parts = clean.split('.');
            const numericParts = [];
            for (const part of parts) {
                if (!/^\d+$/.test(part)) {
                    return null;
                }
                numericParts.push(Number(part));
            }
            return numericParts;
        }

        function compareVersions(leftText, rightText) {
            const left = parseVersionParts(leftText);
            const right = parseVersionParts(rightText);
            if (!left || !right) {
                return 0;
            }

            const maxLength = Math.max(left.length, right.length);
            for (let index = 0; index < maxLength; index++) {
                const leftPart = index < left.length ? left[index] : 0;
                const rightPart = index < right.length ? right[index] : 0;
                if (leftPart < rightPart) {
                    return -1;
                }
                if (leftPart > rightPart) {
                    return 1;
                }
            }

            return 0;
        }

        function normalizeVersion(versionText) {
            const clean = String(versionText || '').trim().replace(/^v/i, '');
            return clean;
        }

        async function loadRepoVersionStatus() {
            const versionNode = document.getElementById('firmwareVersion');
            const statusNode = document.getElementById('versionStatus');
            if (!versionNode || !statusNode) {
                return;
            }

            const currentVersion = normalizeVersion(versionNode.textContent);
            if (!currentVersion) {
                return;
            }

            try {
                const res = await fetch(repoTagsUrl, {
                    headers: { 'Accept': 'application/vnd.github+json' }
                });
                if (!res.ok) {
                    return;
                }

                const tags = await res.json();
                if (!Array.isArray(tags) || tags.length === 0) {
                    return;
                }

                let latestVersion = null;
                for (const tag of tags) {
                    const candidate = normalizeVersion(tag && tag.name ? tag.name : '');
                    if (!candidate || !parseVersionParts(candidate)) {
                        continue;
                    }
                    if (!latestVersion || compareVersions(candidate, latestVersion) > 0) {
                        latestVersion = candidate;
                    }
                }

                if (!latestVersion) {
                    return;
                }

                latestRepoVersion = latestVersion;

                if (compareVersions(currentVersion, latestVersion) >= 0) {
                    statusNode.classList.add('up-to-date');
                    statusNode.textContent = ' (UP TO DATE)';
                } else {
                    statusNode.classList.remove('up-to-date');
                    statusNode.textContent = ' (' + latestVersion + ')';
                }
            } catch (_) {
                // Ignore GitHub version check failures and leave the local version visible.
            }
        }

        function updateNetworkInfo(data) {
            if (!data) return;

            if (data.mode) {
                const select = document.getElementById('networkMode');
                select.value = String(data.mode).toLowerCase();
            }
            if (data.mac) {
                document.getElementById('macAddress').textContent = data.mac;
            }
            if (data.ip) {
                document.getElementById('ipAddress').textContent = data.ip;
            }
        }

        async function loadNetworkMode() {
            try {
                const res = await fetch('/network');
                const data = await res.json();
                if (res.ok && data.status === 'ok') {
                    updateNetworkInfo(data);
                }
            } catch (_) {
                // Ignore network info refresh failures in UI bootstrap.
            }
        }

        async function loadWifiConfig() {
            try {
                const res = await fetch('/network/wifi');
                const data = await res.json();
                if (res.ok && data.status === 'ok') {
                    document.getElementById('wifiSsid').value = data.ssid || '';
                    document.getElementById('wifiPassword').value = data.password || '';
                }
            } catch (_) {
                // Ignore fetch errors here, user can still enter values manually.
            }
        }

        async function saveWifiConfig() {
            const ssid = document.getElementById('wifiSsid').value;
            const password = document.getElementById('wifiPassword').value;

            try {
                const res = await fetch('/network/wifi', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ ssid: ssid, password: password })
                });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Failed to save WiFi config');
                }
                setStatus('WiFi credentials saved.');
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function loadDeviceCredentials() {
            try {
                const res = await fetch('/network/auth');
                const data = await res.json();
                if (res.ok && data.status === 'ok') {
                    document.getElementById('deviceUsername').value = data.username || 'logix';
                    document.getElementById('devicePassword').value = data.password || 'logix';
                    document.getElementById('webUiAuthEnabled').checked = data.webui_auth_enabled !== false;
                }
            } catch (_) {
                // Ignore fetch errors here, user can still enter values manually.
            }
        }

        async function saveDeviceCredentials() {
            const usernameInput = document.getElementById('deviceUsername');
            const passwordInput = document.getElementById('devicePassword');
            const authEnabledInput = document.getElementById('webUiAuthEnabled');
            const username = String(usernameInput.value || '').trim();
            const password = String(passwordInput.value || '');
            const webuiAuthEnabled = Boolean(authEnabledInput.checked);

            if (!username || username.length > 8) {
                setStatus('Username is required and must be 8 characters or less.');
                return;
            }

            if (!password) {
                setStatus('Password is required.');
                return;
            }

            try {
                const res = await fetch('/network/auth', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ username: username, password: password, webui_auth_enabled: webuiAuthEnabled })
                });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Failed to save device credentials');
                }
                usernameInput.value = data.username || username;
                passwordInput.value = data.password || password;
                authEnabledInput.checked = data.webui_auth_enabled !== false;
                setStatus('Device user/password saved.');
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function applyNetworkMode() {
            const mode = document.getElementById('networkMode').value;
            try {
                const res = await fetch('/network/mode', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ mode: mode })
                });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Failed to set network mode');
                }
                updateNetworkInfo(data);
                setStatus('Network mode set to ' + String(data.mode).toUpperCase());
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        function updateQueueButton(enabled) {
            const btn = document.getElementById('queueToggle');
            btn.dataset.enabled = enabled ? '1' : '0';
            btn.textContent = enabled ? 'Play Queued: ON' : 'Play Queued: OFF';
            btn.style.background = enabled ? '#bfdbfe' : '#dbeafe';
        }

        function setQueuePauseInput(seconds) {
            const input = document.getElementById('queuePauseSeconds');
            input.value = String(seconds);
        }

        function setUrlLoopCheckbox(enabled) {
            const input = document.getElementById('urlLoopEnabled');
            input.checked = Boolean(enabled);
        }

        async function loadFiles() {
            try {
                const res = await fetch('/files');
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Failed to list files');
                }

                const rows = document.getElementById('fileRows');
                rows.innerHTML = '';
                updateQueueButton(Boolean(data.queue_enabled));
                setQueuePauseInput(data.queue_pause_seconds || 1);
                setUrlLoopCheckbox(Boolean(data.url_loop_enabled));

                if (!data.files || data.files.length === 0) {
                    const tr = document.createElement('tr');
                    tr.innerHTML = '<td colspan="4">No audio or playlist files found.</td>';
                    rows.appendChild(tr);
                } else {
                    data.files.forEach((file) => {
                        const tr = document.createElement('tr');
                        const safeName = String(file.name);
                        const checked = file.active ? 'checked' : '';
                        const isPlaylist = Boolean(file.is_playlist);
                        const actionButton = isPlaylist
                            ? '<button class="btn btn-play" onclick="togglePlaylist(\'' + safeName + '\', ' + (file.active ? 'true' : 'false') + ')">' + (file.active ? 'Turn Off' : 'Turn On') + '</button>'
                            : '<button class="btn btn-play" onclick="playFile(\'' + safeName + '\')">Play</button>';
                        tr.innerHTML =
                            '<td><input type="checkbox" ' + checked + ' onchange="setActive(\'' + safeName + '\', this.checked)"></td>' +
                            '<td>' + safeName + '</td>' +
                            '<td>' + file.size + '</td>' +
                            '<td>' +
                            actionButton +
                            '<button class="btn btn-del" onclick="deleteFile(\'' + safeName + '\')">Delete</button>' +
                            '</td>';
                        rows.appendChild(tr);
                    });
                }

                setStatus('Loaded ' + data.count + ' file(s).');
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function setActive(name, active) {
            try {
                const res = await fetch('/playlist/item', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ file: name, active: active })
                });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Failed to update active state');
                }
                updateQueueButton(Boolean(data.queue_enabled));
                setStatus((active ? 'Queued ' : 'Removed ') + name);
            } catch (err) {
                setStatus('Error: ' + err.message);
                await loadFiles();
            }
        }

        async function toggleQueue() {
            const btn = document.getElementById('queueToggle');
            const currentlyEnabled = btn.dataset.enabled === '1';
            const next = !currentlyEnabled;

            try {
                const res = await fetch('/playlist/queue', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ enabled: next })
                });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Failed to toggle queue');
                }

                updateQueueButton(Boolean(data.queue_enabled));
                setQueuePauseInput(data.queue_pause_seconds || 1);
                setStatus('Play Queued is now ' + (data.queue_enabled ? 'ON' : 'OFF'));
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function saveQueuePause() {
            const input = document.getElementById('queuePauseSeconds');
            const seconds = Number(input.value);
            if (!Number.isInteger(seconds) || seconds < 1 || seconds > 180) {
                setStatus('Pause must be a whole number from 1 to 180.');
                return;
            }

            try {
                const res = await fetch('/playlist/pause', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ seconds: seconds })
                });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Failed to save queue pause');
                }

                setQueuePauseInput(data.queue_pause_seconds || seconds);
                setStatus('Queue pause set to ' + (data.queue_pause_seconds || seconds) + ' second(s).');
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function playFile(name) {
            try {
                const res = await fetch('/play', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ file: name })
                });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Play failed');
                }
                setStatus('Playing ' + name);
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function playUrl() {
            const url = String(document.getElementById('streamUrl').value || '').trim();
            const loop = document.getElementById('urlLoopEnabled').checked;

            if (!url) {
                setStatus('Enter a stream URL first.');
                return;
            }

            if (!url.startsWith('http://') && !url.startsWith('https://')) {
                setStatus('URL must start with http:// or https://');
                return;
            }

            try {
                const res = await fetch('/play', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ url: url, loop: loop })
                });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'URL play failed');
                }

                setUrlLoopCheckbox(Boolean(data.url_loop_enabled));
                setStatus('Streaming URL' + (data.url_loop_enabled ? ' (loop ON)' : ' (loop OFF)'));
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function stopPlayback() {
            try {
                const res = await fetch('/stop', { method: 'POST' });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Stop failed');
                }
                setStatus('Playback stopped.');
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function togglePlaylist(name, currentlyActive) {
            try {
                const next = !currentlyActive;
                const res = await fetch('/playlist/item', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ file: name, active: next })
                });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Failed to toggle playlist');
                }

                if (next) {
                    const queueRes = await fetch('/playlist/queue', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ enabled: true })
                    });
                    const queueData = await queueRes.json();
                    if (!queueRes.ok || queueData.status !== 'ok') {
                        throw new Error(queueData.message || 'Failed to start playlist');
                    }
                    updateQueueButton(Boolean(queueData.queue_enabled));
                } else {
                    const queueRes = await fetch('/playlist/queue', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ enabled: false })
                    });
                    const queueData = await queueRes.json();
                    if (!queueRes.ok || queueData.status !== 'ok') {
                        throw new Error(queueData.message || 'Failed to stop playlist');
                    }
                    updateQueueButton(Boolean(queueData.queue_enabled));
                }

                setStatus((next ? 'Turned ON ' : 'Turned OFF ') + name);
                await loadFiles();
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function deleteFile(name) {
            if (!confirm('Delete ' + name + '?')) {
                return;
            }

            try {
                const res = await fetch('/files?name=' + encodeURIComponent(name), { method: 'DELETE' });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Delete failed');
                }
                setStatus('Deleted ' + name);
                await loadFiles();
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function uploadFile() {
            const input = document.getElementById('audioFile');
            if (!input.files || input.files.length === 0) {
                setStatus('Select an MP3, WAV, or M3U file first.');
                return;
            }

            const formData = new FormData();
            formData.append('file', input.files[0]);

            try {
                const res = await fetch('/files/upload', {
                    method: 'POST',
                    body: formData
                });
                const data = await res.json();
                if (!res.ok || data.status !== 'ok') {
                    throw new Error(data.message || 'Upload failed');
                }
                setStatus('Uploaded ' + data.file);
                input.value = '';
                await loadFiles();
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        async function loadVolume() {
            try {
                const res = await fetch('/volume');
                const data = await res.json();
                if (res.ok && typeof data.volume === 'number') {
                    document.getElementById('volumeSlider').value = data.volume;
                    document.getElementById('volumeLabel').textContent = data.volume;
                }
            } catch (err) {
                // non-fatal
            }
        }

        async function setVolume(value) {
            try {
                const res = await fetch('/volume', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ volume: parseInt(value, 10) })
                });
                const data = await res.json();
                if (res.ok && typeof data.volume === 'number') {
                    document.getElementById('volumeSlider').value = data.volume;
                    document.getElementById('volumeLabel').textContent = data.volume;
                }
            } catch (err) {
                setStatus('Error: ' + err.message);
            }
        }

        loadNetworkMode();
        loadWifiConfig();
        loadDeviceCredentials();
        loadFiles();
        loadVolume();
        loadRepoVersionStatus();
        document.getElementById('firmwareVersion').addEventListener('click', handleVersionClick);
    </script>
</body>
</html>
)HTML";
        html.replace("__MAC__", mac);
        html.replace("__IP__", ip);
    html.replace("__VERSION__", version);
        html.replace("__MODE__", mode);
        html.replace("__SD_USED__", sd_used_str);
        html.replace("__SD_FREE__", sd_free_str);

    server.send(200, "text/html", html);
}

static void handleDeleteFile() {
        if (!server.hasArg("name")) {
                sendJson(400, "error", "Missing 'name' query parameter");
                return;
        }

        char path[128];
        if (!normalizeFilename(server.arg("name").c_str(), path, sizeof(path))) {
                sendJson(400, "error", "Invalid filename");
                return;
        }

        if (!hasSupportedLibraryFileExtension(path)) {
            sendJson(400, "error", "Only .mp3, .wav, .m3u or .m3u8 files are supported");
                return;
        }

        if (!SD.exists(path)) {
                sendJson(404, "error", "File not found");
                return;
        }

        if (!SD.remove(path)) {
                sendJson(500, "error", "Failed to delete file");
                return;
        }

        setFileActiveInPlaylist(String(path + 1), false);
        savePlaylistState();

        JsonDocument resp;
        resp["status"] = "ok";
        resp["file"] = path + 1;
        String body;
        serializeJson(resp, body);
        server.send(200, "application/json", body);
}

static void handleUploadFileData() {
        HTTPUpload& upload = server.upload();

        if (upload.status == UPLOAD_FILE_START) {
                if (upload_file) {
                        upload_file.close();
                }

                upload_failed = false;
                upload_status_code = 200;
                upload_error = "";
                upload_saved_name = "";

                String name = upload.filename;
                int slash = name.lastIndexOf('/');
                int backslash = name.lastIndexOf('\\');
                int cut = slash > backslash ? slash : backslash;
                if (cut >= 0) {
                        name = name.substring(cut + 1);
                }

                char path[128];
                if (!normalizeFilename(name.c_str(), path, sizeof(path))) {
                        upload_failed = true;
                        upload_status_code = 400;
                        upload_error = "Invalid filename";
                        return;
                }

                if (!hasSupportedLibraryFileExtension(path)) {
                        upload_failed = true;
                        upload_status_code = 400;
                    upload_error = "Only .mp3, .wav, .m3u or .m3u8 files are supported";
                        return;
                }

                if (SD.exists(path)) {
                        SD.remove(path);
                }

                upload_file = SD.open(path, FILE_WRITE);
                if (!upload_file) {
                        upload_failed = true;
                        upload_status_code = 500;
                        upload_error = "Failed to open file for upload";
                        return;
                }

                upload_saved_name = String(path + 1);
        } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (upload_failed || !upload_file) {
                        return;
                }

                size_t written = upload_file.write(upload.buf, upload.currentSize);
                if (written != upload.currentSize) {
                        upload_failed = true;
                        upload_status_code = 500;
                        upload_error = "Failed while writing upload data";
                        upload_file.close();
                }
        } else if (upload.status == UPLOAD_FILE_END) {
                if (upload_file) {
                        upload_file.close();
                }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
                if (upload_file) {
                        upload_file.close();
                }
                upload_failed = true;
                upload_status_code = 500;
                upload_error = "Upload aborted";
        }
}

static void handleUploadFileComplete() {
        if (upload_file) {
                upload_file.close();
        }

        if (upload_failed) {
                sendJson(upload_status_code, "error", upload_error.c_str());
                return;
        }

        JsonDocument resp;
        resp["status"] = "ok";
        resp["file"] = upload_saved_name;
        String body;
        serializeJson(resp, body);
        server.send(200, "application/json", body);
}

    static void handleFirmwareUploadData() {
        HTTPUpload& upload = server.upload();

        if (upload.status == UPLOAD_FILE_START) {
            firmware_upload_failed = false;
            firmware_upload_status_code = 200;
            firmware_upload_error = "";
            firmware_uploaded_version = "";

            String name = upload.filename;
            int slash = name.lastIndexOf('/');
            int backslash = name.lastIndexOf('\\');
            int cut = slash > backslash ? slash : backslash;
            if (cut >= 0) {
                name = name.substring(cut + 1);
            }

            if (!hasFirmwareExtension(name.c_str())) {
                firmware_upload_failed = true;
                firmware_upload_status_code = 400;
                firmware_upload_error = "Only .bin firmware files are supported";
                return;
            }

            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                firmware_upload_failed = true;
                firmware_upload_status_code = 500;
                firmware_upload_error = Update.errorString();
                return;
            }

            firmware_uploaded_version = name;
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (firmware_upload_failed) {
                return;
            }

            size_t written = Update.write(upload.buf, upload.currentSize);
            if (written != upload.currentSize) {
                firmware_upload_failed = true;
                firmware_upload_status_code = 500;
                firmware_upload_error = Update.errorString();
                Update.abort();
                return;
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (firmware_upload_failed) {
                return;
            }

            if (!Update.end(true)) {
                firmware_upload_failed = true;
                firmware_upload_status_code = 500;
                firmware_upload_error = Update.errorString();
                return;
            }

            if (!Update.isFinished()) {
                firmware_upload_failed = true;
                firmware_upload_status_code = 500;
                firmware_upload_error = "Firmware update did not finish";
                return;
            }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
            firmware_upload_failed = true;
            firmware_upload_status_code = 500;
            firmware_upload_error = "Firmware upload aborted";
            Update.abort();
        }
    }

    static void handleFirmwareUploadComplete() {
        if (firmware_upload_failed) {
            sendJson(firmware_upload_status_code, "error", firmware_upload_error.c_str());
            return;
        }

        firmware_reboot_pending = true;
        firmware_reboot_at_ms = millis() + 1500;

        JsonDocument resp;
        resp["status"] = "ok";
        resp["message"] = "Firmware uploaded. Device will reboot shortly.";
        resp["file"] = firmware_uploaded_version;
        sendJsonDocument(200, resp);
    }

// ─────────────────────────────────────────────
//  POST /play
//    { "file": "announcement.mp3" }           — play from SD card
//    { "url":  "http://host/stream.mp3" }     — play from HTTP/HTTPS URL
// ─────────────────────────────────────────────
static void handlePlay() {
    if (!server.hasArg("plain")) {
        sendJson(400, "error", "No JSON body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        sendJson(400, "error", "Invalid JSON");
        return;
    }

    const char* url = doc["url"] | "";
    if (strlen(url) > 0) {
        bool loop_url = doc["loop"].is<bool>() ? doc["loop"].as<bool>() : url_loop_enabled;

        if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
            sendJson(400, "error", "URL must start with http:// or https://");
            return;
        }

        queued_play_enabled = false;
        queued_play_index = 0;
        queue_idle_since_ms = 0;
        url_loop_enabled = loop_url;
        url_loop_armed = loop_url;
        last_stream_url = url;
        url_loop_idle_since_ms = 0;
        next_url_loop_attempt_ms = 0;

        if (!audioPlayUrl(url, loop_url)) {
            url_loop_armed = false;
            sendJson(500, "error", "URL playback failed");
            return;
        }

        if (!savePlaylistState()) {
            sendJson(500, "error", "Failed to persist playlist state");
            return;
        }

        JsonDocument resp;
        resp["status"] = "ok";
        resp["url"] = url;
        resp["url_loop_enabled"] = url_loop_enabled;
        String body;
        serializeJson(resp, body);
        server.send(200, "application/json", body);
        return;
    }

    const char* filename = doc["file"] | "";
    if (strlen(filename) == 0) {
        sendJson(400, "error", "Missing 'file' or 'url' field");
        return;
    }

    // Reject path traversal attempts
    if (strstr(filename, "..") != nullptr) {
        sendJson(400, "error", "Invalid filename");
        return;
    }

    // Verify file exists before attempting playback
    char path[128];
    snprintf(path, sizeof(path), filename[0] == '/' ? "%s" : "/%s", filename);

    if (!hasSupportedLibraryFileExtension(path)) {
        sendJson(400, "error", "Only .mp3, .wav, .m3u or .m3u8 files are supported");
        return;
    }

    if (!SD.exists(path)) {
        sendJson(404, "error", "File not found");
        return;
    }

    if (hasPlaylistExtension(path)) {
        url_loop_armed = false;
        last_stream_url = "";
        url_loop_idle_since_ms = 0;
        next_url_loop_attempt_ms = 0;

        String playlist_error;
        if (!loadQueueFromM3uFile(path, &playlist_error)) {
            sendJson(400, "error", playlist_error.c_str());
            return;
        }

        audioStop();
        if (!savePlaylistState()) {
            sendJson(500, "error", "Failed to persist playlist state");
            return;
        }
    } else {
        url_loop_armed = false;
        last_stream_url = "";
        url_loop_idle_since_ms = 0;
        next_url_loop_attempt_ms = 0;

        if (!audioPlayFile(filename)) {
            sendJson(500, "error", "Playback failed");
            return;
        }
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["file"]   = filename;
    if (hasPlaylistExtension(path)) {
        resp["queue_enabled"] = queued_play_enabled;
        resp["active_count"] = active_playlist_count;
    }
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
    //Serial.printf("API: Play request -> %s\n", filename);
}

// ─────────────────────────────────────────────
//  POST /stop
// ─────────────────────────────────────────────
static void handleStop() {
    audioStop();
    queued_play_enabled = false;
    queued_play_index = 0;
    queue_idle_since_ms = 0;
    url_loop_armed = false;
    last_stream_url = "";
    url_loop_idle_since_ms = 0;
    next_url_loop_attempt_ms = 0;
    savePlaylistState();
    sendJson(200, "ok", "Stopped");
    //Serial.println("API: Stop request");
}

// ─────────────────────────────────────────────
//  GET /status
// ─────────────────────────────────────────────
static void handleStatus() {
    JsonDocument doc;
    char audio_file_label[22];
    buildAudioFileLabel(audio_file_label, sizeof(audio_file_label));

    doc["status"]  = "ok";
    doc["playing"] = audioIsPlaying();
    doc["audio_state"] = getAudioStateLabel();
    doc["volume"]  = audioGetVolume();
    doc["audio_file_label"] = audio_file_label[0] ? audio_file_label : "---";
    doc["queue_enabled"] = queued_play_enabled;
    doc["active_count"] = active_playlist_count;
    doc["queue_pause_seconds"] = queue_pause_seconds;
    doc["url_loop_enabled"] = url_loop_enabled;
    doc["url_loop_armed"] = url_loop_armed;
    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  GET /network
// ─────────────────────────────────────────────
static void handleGetNetwork() {
    updateLastNetworkSnapshot();

    JsonDocument resp;
    resp["status"] = "ok";
    resp["mode"] = getNetworkModeString();
    resp["connected"] = isNetworkConnected();
    resp["ip"] = getNetworkIpAddress();
    resp["mac"] = getNetworkMacAddress();
    resp["last_mode"] = last_network_mode;
    resp["last_connected"] = last_network_connected;
    resp["last_ip"] = last_network_ip;
    resp["last_mac"] = last_network_mac;
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  POST /network/mode  { "mode": "eth"|"wifi" }
// ─────────────────────────────────────────────
static void handleSetNetworkMode() {
    if (!server.hasArg("plain")) {
        sendJson(400, "error", "No JSON body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        sendJson(400, "error", "Invalid JSON");
        return;
    }

    String mode = doc["mode"] | "";
    mode.toLowerCase();
    NetworkMode target_mode;
    if (mode == "eth") {
        target_mode = NETWORK_MODE_ETH;
    } else if (mode == "wifi") {
        target_mode = NETWORK_MODE_WIFI;
    } else {
        sendJson(400, "error", "Invalid mode, use 'eth' or 'wifi'");
        return;
    }

    if (!setNetworkMode(target_mode)) {
        sendJson(500, "error", "Failed to switch network mode");
        return;
    }

    updateLastNetworkSnapshot();
    savePlaylistState();

    JsonDocument resp;
    resp["status"] = "ok";
    resp["mode"] = getNetworkModeString();
    resp["connected"] = isNetworkConnected();
    resp["ip"] = getNetworkIpAddress();
    resp["mac"] = getNetworkMacAddress();
    resp["last_mode"] = last_network_mode;
    resp["last_connected"] = last_network_connected;
    resp["last_ip"] = last_network_ip;
    resp["last_mac"] = last_network_mac;
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  GET /network/wifi
// ─────────────────────────────────────────────
static void handleGetWifiConfig() {
    JsonDocument resp;
    resp["status"] = "ok";
    resp["ssid"] = getWifiSsid();
    resp["password"] = getWifiPassword();
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  POST /network/wifi  { "ssid": "...", "password": "..." }
// ─────────────────────────────────────────────
static void handleSetWifiConfig() {
    if (!server.hasArg("plain")) {
        sendJson(400, "error", "No JSON body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        sendJson(400, "error", "Invalid JSON");
        return;
    }

    if (!doc["ssid"].is<const char*>() || !doc["password"].is<const char*>()) {
        sendJson(400, "error", "Missing or invalid WiFi fields");
        return;
    }

    String ssid = doc["ssid"].as<String>();
    String password = doc["password"].as<String>();
    bool applied = setWifiCredentials(ssid, password);

    if (!savePlaylistState()) {
        sendJson(500, "error", "Failed to persist playlist state");
        return;
    }

    if (!applied) {
        sendJson(500, "error", "Failed to apply WiFi credentials");
        return;
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["ssid"] = getWifiSsid();
    resp["password"] = getWifiPassword();
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  GET /network/auth
// ─────────────────────────────────────────────
static void handleGetDeviceCredentials() {
    JsonDocument resp;
    resp["status"] = "ok";
    resp["username"] = device_username;
    resp["password"] = device_password;
    resp["webui_auth_enabled"] = webui_auth_enabled;
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  POST /network/auth  { "username": "...", "password": "..." }
// ─────────────────────────────────────────────
static void handleSetDeviceCredentials() {
    if (!server.hasArg("plain")) {
        sendJson(400, "error", "No JSON body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        sendJson(400, "error", "Invalid JSON");
        return;
    }

    if (!doc["username"].is<const char*>() || !doc["password"].is<const char*>()) {
        sendJson(400, "error", "Missing or invalid username/password fields");
        return;
    }

    String requested_username = doc["username"].as<String>();
    normalizeDeviceUsername(requested_username);
    if (requested_username.length() == 0 || requested_username.length() > 8) {
        sendJson(400, "error", "Username must be 1 to 8 characters");
        return;
    }

    String requested_password = doc["password"].as<String>();
    if (requested_password.length() == 0) {
        sendJson(400, "error", "Password is required");
        return;
    }

    bool requested_auth_enabled = doc["webui_auth_enabled"].is<bool>()
        ? doc["webui_auth_enabled"].as<bool>()
        : webui_auth_enabled;

    device_username = requested_username;
    device_password = requested_password;
    webui_auth_enabled = requested_auth_enabled;

    if (!saveDeviceCredentialsToPrefs()) {
        sendJson(500, "error", "Failed to persist device credentials");
        return;
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["username"] = device_username;
    resp["password"] = device_password;
    resp["webui_auth_enabled"] = webui_auth_enabled;
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  GET /playlist/pause
// ─────────────────────────────────────────────
static void handlePlaylistPauseGet() {
    JsonDocument resp;
    resp["status"] = "ok";
    resp["queue_pause_seconds"] = queue_pause_seconds;
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  POST /playlist/pause  { "seconds": 1..180 }
// ─────────────────────────────────────────────
static void handlePlaylistPauseSet() {
    if (!server.hasArg("plain")) {
        sendJson(400, "error", "No JSON body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        sendJson(400, "error", "Invalid JSON");
        return;
    }

    if (!doc["seconds"].is<int>()) {
        sendJson(400, "error", "Missing or invalid 'seconds' field");
        return;
    }

    int seconds = doc["seconds"].as<int>();
    if (seconds < 1 || seconds > 180) {
        sendJson(400, "error", "Pause out of range (1-180)");
        return;
    }

    queue_pause_seconds = static_cast<uint16_t>(seconds);
    if (!savePlaylistState()) {
        sendJson(500, "error", "Failed to persist playlist state");
        return;
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["queue_pause_seconds"] = queue_pause_seconds;
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  GET /version
// ─────────────────────────────────────────────
static void handleVersion() {
    JsonDocument doc;
    doc["status"] = "ok";
    doc["version"] = BUILD_VERSION;
    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  POST /volume  { "volume": 0..100 }
// ─────────────────────────────────────────────
static void handleVolume() {
    if (!server.hasArg("plain")) {
        sendJson(400, "error", "No JSON body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        sendJson(400, "error", "Invalid JSON");
        return;
    }

    if (!doc["volume"].is<int>()) {
        sendJson(400, "error", "Missing or invalid 'volume' field");
        return;
    }

    int value = doc["volume"].as<int>();
    if (value < 0 || value > 100) {
        sendJson(400, "error", "Volume out of range (0-100)");
        return;
    }

    if (!audioSetVolume(static_cast<uint8_t>(value))) {
        sendJson(500, "error", "Failed to set volume");
        return;
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["volume"] = audioGetVolume();
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
    //Serial.printf("API: Volume request -> %d\n", value);
}

// ─────────────────────────────────────────────
//  GET /volume
// ─────────────────────────────────────────────
static void handleGetVolume() {
    JsonDocument resp;
    resp["status"] = "ok";
    resp["volume"] = audioGetVolume();
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  POST /playlist/item  { "file": "x.mp3|x.wav|x.m3u", "active": true }
// ─────────────────────────────────────────────
static void handlePlaylistItem() {
    if (!server.hasArg("plain")) {
        sendJson(400, "error", "No JSON body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        sendJson(400, "error", "Invalid JSON");
        return;
    }

    const char* file = doc["file"] | "";
    if (strlen(file) == 0 || !doc["active"].is<bool>()) {
        sendJson(400, "error", "Missing or invalid fields");
        return;
    }

    char path[128];
    if (!normalizeFilename(file, path, sizeof(path))) {
        sendJson(400, "error", "Invalid filename");
        return;
    }

    bool active = doc["active"].as<bool>();
    String normalized = String(path + 1);

    if (hasPlaylistExtension(path)) {
        if (!SD.exists(path)) {
            sendJson(404, "error", "File not found");
            return;
        }
        String tracks[64];
        size_t track_count = parseM3uTracks(path, tracks, 64);
        if (active && track_count == 0) {
            sendJson(400, "error", "Playlist has no playable entries");
            return;
        }
        for (size_t i = 0; i < track_count; ++i) {
            setFileActiveInPlaylist(tracks[i], active);
        }

        if (active) {
            queued_play_enabled = active_playlist_count > 0;
            if (queued_play_enabled && !audioIsPlaying()) {
                queue_idle_since_ms = millis() - 500;
            }
        } else {
            queued_play_enabled = false;
            queued_play_index = 0;
            queue_idle_since_ms = 0;
        }
    } else {
        if (!hasSupportedAudioExtension(path)) {
            sendJson(400, "error", "Only .mp3 or .wav files are supported");
            return;
        }
        if (active && !SD.exists(path)) {
            sendJson(404, "error", "File not found");
            return;
        }
        if (!setFileActiveInPlaylist(normalized, active)) {
            sendJson(500, "error", "Playlist is full");
            return;
        }
    }

    if (!savePlaylistState()) {
        sendJson(500, "error", "Failed to persist playlist state");
        return;
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["file"] = normalized;
    resp["active"] = active;
    resp["queue_enabled"] = queued_play_enabled;
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  POST /playlist/queue  { "enabled": true|false }
// ─────────────────────────────────────────────
static void handlePlaylistQueue() {
    if (!server.hasArg("plain")) {
        sendJson(400, "error", "No JSON body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        sendJson(400, "error", "Invalid JSON");
        return;
    }

    if (!doc["enabled"].is<bool>()) {
        sendJson(400, "error", "Missing or invalid 'enabled' field");
        return;
    }

    queued_play_enabled = doc["enabled"].as<bool>() && active_playlist_count > 0;
    if (!queued_play_enabled) {
        queued_play_index = 0;
        queue_idle_since_ms = 0;
    } else if (!audioIsPlaying()) {
        // Start quickly when enabled while idle.
        queue_idle_since_ms = millis() - 500;
    }

    if (!savePlaylistState()) {
        sendJson(500, "error", "Failed to persist playlist state");
        return;
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["queue_enabled"] = queued_play_enabled;
    resp["active_count"] = active_playlist_count;
    resp["queue_pause_seconds"] = queue_pause_seconds;
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  POST /playlist/start
// ─────────────────────────────────────────────
static void handlePlaylistStart() {
    if (active_playlist_count == 0) {
        sendJson(400, "error", "No active files in playlist");
        return;
    }

    queued_play_enabled = true;
    if (!audioIsPlaying()) {
        // Start quickly when queue is enabled while idle.
        queue_idle_since_ms = millis() - 500;
    }

    if (!savePlaylistState()) {
        sendJson(500, "error", "Failed to persist playlist state");
        return;
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["queue_enabled"] = queued_play_enabled;
    resp["active_count"] = active_playlist_count;
    resp["queue_pause_seconds"] = queue_pause_seconds;
    String body;
    serializeJson(resp, body);
    server.send(200, "application/json", body);
}

// ─────────────────────────────────────────────
//  GET /files
// ─────────────────────────────────────────────
static void handleFiles() {
    File root = SD.open("/");
    if (!root) {
        sendJson(500, "error", "Failed to open SD root");
        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["queue_enabled"] = queued_play_enabled;
    doc["active_count"] = active_playlist_count;
    doc["queue_pause_seconds"] = queue_pause_seconds;
    doc["url_loop_enabled"] = url_loop_enabled;
    JsonArray files = doc["files"].to<JsonArray>();
    int count = 0;

    while (true) {
        File entry = root.openNextFile();
        if (!entry) {
            break;
        }

        if (!entry.isDirectory()) {
            String name = String(entry.name());
            String lowered = name;
            lowered.toLowerCase();

            if (lowered == "playlist.m3u") {
                entry.close();
                continue;
            }

            const bool is_audio = lowered.endsWith(".mp3") || lowered.endsWith(".wav");
            const bool is_playlist = lowered.endsWith(".m3u") || lowered.endsWith(".m3u8");

            if (is_audio || is_playlist) {
                JsonObject item = files.add<JsonObject>();
                item["name"] = name;
                item["size"] = entry.size();
                item["is_playlist"] = is_playlist;
                if (is_playlist) {
                    char pl_path[128];
                    normalizeFilename(name.c_str(), pl_path, sizeof(pl_path));
                    String tracks[64];
                    size_t tc = parseM3uTracks(pl_path, tracks, 64);
                    bool any_active = false;
                    for (size_t j = 0; j < tc && !any_active; ++j) {
                        any_active = isFileActiveInPlaylist(tracks[j]);
                    }
                    item["active"] = any_active;
                } else {
                    item["active"] = isFileActiveInPlaylist(name);
                }
                item["queue_selectable"] = true;
                count++;
            }
        }

        entry.close();
    }

    root.close();
    doc["count"] = count;

    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
}

static void handleNotFound() {
    sendJson(404, "error", "Not found");
}

// ─────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────
void initApiServer() {
    if (server_running) {
        return;
    }

    loadPlaylistState();
    loadDeviceCredentialsFromPrefs();

    // Keep docs/openapi.yaml and docs/api_sync_checklist.md aligned with route changes.
    server.on("/",      HTTP_GET,  handleWebUi);
    server.on("/play",   HTTP_POST, handlePlay);
    server.on("/stop",   HTTP_POST, handleStop);
    server.on("/volume", HTTP_POST, handleVolume);
    server.on("/volume", HTTP_GET,  handleGetVolume);
    server.on("/network", HTTP_GET, handleGetNetwork);
    server.on("/network/mode", HTTP_POST, handleSetNetworkMode);
    server.on("/network/wifi", HTTP_GET, handleGetWifiConfig);
    server.on("/network/wifi", HTTP_POST, handleSetWifiConfig);
    server.on("/network/auth", HTTP_GET, handleGetDeviceCredentials);
    server.on("/network/auth", HTTP_POST, handleSetDeviceCredentials);
    server.on("/version", HTTP_GET, handleVersion);
    server.on("/status", HTTP_GET,  handleStatus);
    server.on("/files",  HTTP_GET,  handleFiles);
    server.on("/files",  HTTP_DELETE, handleDeleteFile);
    server.on("/files/upload", HTTP_POST, handleUploadFileComplete, handleUploadFileData);
    server.on("/firmware/update", HTTP_POST, handleFirmwareUploadComplete, handleFirmwareUploadData);
    server.on("/playlist/item", HTTP_POST, handlePlaylistItem);
    server.on("/playlist/queue", HTTP_POST, handlePlaylistQueue);
    server.on("/playlist/start", HTTP_POST, handlePlaylistStart);
    server.on("/playlist/pause", HTTP_GET, handlePlaylistPauseGet);
    server.on("/playlist/pause", HTTP_POST, handlePlaylistPauseSet);
    server.onNotFound(handleNotFound);
    server.begin();
    server_running = true;
    //Serial.println("API: HTTP server started on port 80");
}

void handleApiServer() {
    if (!server_running) {
        return;
    }

    serviceQueuedPlayback();
    serviceUrlLoopPlayback();
    server.handleClient();

    if (firmware_reboot_pending && millis() >= firmware_reboot_at_ms) {
        delay(100);
        ESP.restart();
    }
}

bool isApiServerRunning() {
    return server_running;
}

bool isQueueEnabled() {
    return queued_play_enabled;
}

void factoryResetCredentials() {
    // Reset web UI auth to defaults and disable the requirement
    device_username = "logix";
    device_password = "logix";
    webui_auth_enabled = false;
    saveDeviceCredentialsToPrefs();

    // Clear WiFi credentials and switch back to Ethernet
    setWifiCredentials("", "");
    setNetworkMode(NETWORK_MODE_ETH);

    // Persist cleared network state to SD
    savePlaylistState();
}
