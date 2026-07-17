// KaraFun (.kfn) support for felixchirp.
//
// This is the C++ port of karaoke-library.js's KaraokePlayer, adapted to
// felixchirp's existing audio pipeline instead of HTMLAudioElement:
//   - The "vocal" (or the only) track rides on the existing g_iv.music /
//     g_iv.chunk playback path, so pause/resume/stop/repeat/progress-bar
//     all keep working unchanged (see iv_play_audio in felixchirp.cpp).
//   - An optional "backing" track plays alongside as an independent
//     Mix_Chunk on its own channel, mirroring the JS player's separate
//     audioVocal / audioBacking elements with independent mute.
//   - song.ini parsing (title/artist/textN/syncN → word-level lyrics +
//     millisecond sync times) is a straight port of parseINI().
#include "felixchirp.h"
#include "kfn.h"
#include "../gl_renderer.h"
#include "../sdl_renderer.h"
#include "../ft_font.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctype.h>
#include <algorithm>
#include <map>
#include <sstream>

extern int g_font_size;

// ============================================================================
// HELPERS
// ============================================================================

static std::string kfn_trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string kfn_lower(const std::string &s) {
    std::string r = s;
    for (auto &c : r) c = (char)tolower((unsigned char)c);
    return r;
}

// True if the whole string (after `prefixLen` chars) is digits — used to
// recognize "text3" / "sync12" style INI keys, same as the JS /^text\d+$/.
static bool kfn_all_digits(const std::string &s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

bool is_kfn_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8] = {};
    for (int i = 0; i < 7 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    return strcmp(ext, ".kfn") == 0;
}

static bool iv_kfn_is_audio_name(const std::string &name) {
    static const char *exts[] = { ".mp3", ".ogg", ".wav", ".m4a", ".aac" };
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = kfn_lower(name.substr(dot));
    for (auto *e : exts) if (ext == e) return true;
    return false;
}

// ============================================================================
// song.ini PARSING  (port of KaraokePlayer.parseINI)
// ============================================================================

static void iv_kfn_parse_ini(const std::string &content) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= content.size(); i++) {
        if (i == content.size() || content[i] == '\n') {
            lines.push_back(content.substr(start, i - start));
            start = i + 1;
        }
    }

    std::string title  = "Unknown Song";
    std::string artist = "Unknown Artist";
    std::map<int, std::string>      textMap;
    std::map<int, std::vector<int>> syncMap;
    std::string currentSection;

    for (auto &raw : lines) {
        std::string line = kfn_trim(raw);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            currentSection = kfn_lower(line.substr(1, line.size() - 2));
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        std::string keyLower = kfn_lower(key);

        if (currentSection == "general" || currentSection.empty()) {
            if (keyLower == "title")  title  = value;
            if (keyLower == "artist") artist = value;
        }

        if (keyLower.rfind("text", 0) == 0 && kfn_all_digits(keyLower, 4)) {
            int idx = atoi(keyLower.c_str() + 4);
            textMap[idx] = value;
        }
        if (keyLower.rfind("sync", 0) == 0 && kfn_all_digits(keyLower, 4)) {
            int idx = atoi(keyLower.c_str() + 4);
            std::vector<int> times;
            size_t pos = 0;
            while (pos <= value.size()) {
                size_t comma = value.find(',', pos);
                std::string tok = (comma == std::string::npos) ? value.substr(pos) : value.substr(pos, comma - pos);
                times.push_back(atoi(tok.c_str()) * 10);  // matches JS: parseInt(v) * 10
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            syncMap[idx] = times;
        }
    }

    g_iv.kfn_lyrics.clear();
    g_iv.kfn_lyrics_lines.clear();
    g_iv.kfn_line_indices.clear();
    g_iv.kfn_sync_times.clear();

    // Concatenate sync0, sync1, sync2... in order, stopping at the first gap.
    std::vector<int> allTimings;
    for (int i = 0; syncMap.count(i); i++) {
        auto &v = syncMap[i];
        allTimings.insert(allTimings.end(), v.begin(), v.end());
    }

    size_t timingIdx = 0;
    for (int i = 0; textMap.count(i); i++) {
        const std::string &line = textMap[i];

        // Split on '/' into syllables, then each syllable on whitespace
        // into words — same two-pass split as the JS version.
        std::vector<std::string> words;
        {
            size_t pos = 0;
            while (pos <= line.size()) {
                size_t slash = line.find('/', pos);
                std::string syl = (slash == std::string::npos) ? line.substr(pos) : line.substr(pos, slash - pos);
                if (!syl.empty()) {
                    std::istringstream iss(syl);
                    std::string w;
                    while (iss >> w) words.push_back(w);
                }
                if (slash == std::string::npos) break;
                pos = slash + 1;
            }
        }

        if (!words.empty()) {
            g_iv.kfn_line_indices.push_back((int)g_iv.kfn_lyrics.size());
            std::string displayLine = line;
            displayLine.erase(std::remove(displayLine.begin(), displayLine.end(), '/'), displayLine.end());
            g_iv.kfn_lyrics_lines.push_back(displayLine);

            for (auto &w : words) {
                g_iv.kfn_lyrics.push_back(w);
                double t = (timingIdx < allTimings.size()) ? (allTimings[timingIdx] / 1000.0) : 0.0;
                g_iv.kfn_sync_times.push_back(t);
                timingIdx++;
            }
        }
    }

    g_iv.kfn_title  = title;
    g_iv.kfn_artist = artist;
}

// ============================================================================
// LOAD / STOP
// ============================================================================

void iv_kfn_stop() {
    if (g_iv.kfn_channel_backing >= 0) {
        Mix_HaltChannel(g_iv.kfn_channel_backing);
        g_iv.kfn_channel_backing = -1;
    }
    if (g_iv.kfn_chunk_backing) {
        Mix_FreeChunk(g_iv.kfn_chunk_backing);
        g_iv.kfn_chunk_backing = nullptr;
    }
    if (!g_iv.kfn_tmp_vocal.empty())   { iv_delete_tempfile(g_iv.kfn_tmp_vocal.c_str());   g_iv.kfn_tmp_vocal.clear(); }
    if (!g_iv.kfn_tmp_backing.empty()) { iv_delete_tempfile(g_iv.kfn_tmp_backing.c_str()); g_iv.kfn_tmp_backing.clear(); }

    g_iv.kfn_active = false;
    g_iv.kfn_title.clear();
    g_iv.kfn_artist.clear();
    g_iv.kfn_lyrics.clear();
    g_iv.kfn_lyrics_lines.clear();
    g_iv.kfn_line_indices.clear();
    g_iv.kfn_sync_times.clear();
    g_iv.kfn_current_word   = 0;
    g_iv.kfn_vocal_muted    = false;
    g_iv.kfn_backing_muted  = false;
}

bool iv_kfn_load(const char *kfn_path, const char *label) {
    iv_kfn_stop();
    iv_stop_audio();
    iv_cdg_free();
    iv_free_tex();
    iv_ensure_mixer();

    KFNArchive archive;
    if (!archive.open(kfn_path)) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: %s", archive.lastError().c_str());
        return false;
    }

    const KFNEntry *ini_entry = archive.find("song.ini");
    if (!ini_entry) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: no song.ini inside archive");
        return false;
    }
    size_t ini_sz = 0;
    unsigned char *ini_buf = archive.extract(*ini_entry, ini_sz);
    if (!ini_buf) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: %s", archive.lastError().c_str());
        return false;
    }
    iv_kfn_parse_ini(std::string((const char *)ini_buf, ini_sz));
    free(ini_buf);

    // Classify audio entries: anything with "instru" or "beat" in the name
    // is the backing track (same heuristic as createAudioElement's isVocal
    // check); the first remaining track is the vocal/primary track.
    const KFNEntry *vocal_entry = nullptr, *backing_entry = nullptr;
    for (auto &e : archive.entries()) {
        if (!iv_kfn_is_audio_name(e.filename)) continue;
        std::string lower = kfn_lower(e.filename);
        bool looksBacking = lower.find("instru") != std::string::npos ||
                             lower.find("beat")   != std::string::npos;
        if (looksBacking) { if (!backing_entry) backing_entry = &e; }
        else               { if (!vocal_entry)  vocal_entry  = &e; }
    }
    if (!vocal_entry) vocal_entry = backing_entry;  // only one track total
    if (backing_entry == vocal_entry) backing_entry = nullptr;

    if (!vocal_entry) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: no audio tracks found");
        return false;
    }

    auto ext_of = [](const std::string &name) -> std::string {
        size_t dot = name.find_last_of('.');
        return (dot == std::string::npos) ? std::string(".mp3") : name.substr(dot);
    };

    std::string tmp_vocal = archive.extractToTemp(*vocal_entry, ext_of(vocal_entry->filename).c_str());
    if (tmp_vocal.empty()) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: failed to extract %s (%s)",
                 vocal_entry->filename.c_str(), archive.lastError().c_str());
        return false;
    }

    // Play the vocal/primary track through the normal pipeline so
    // pause/resume/stop/repeat/the progress bar all keep working.
    iv_play_audio(tmp_vocal.c_str(), label, false);
    if (!g_iv.audio_playing) {
        iv_delete_tempfile(tmp_vocal.c_str());
        return false;
    }
    g_iv.kfn_tmp_vocal = tmp_vocal;

    if (backing_entry) {
        std::string tmp_backing = archive.extractToTemp(*backing_entry, ext_of(backing_entry->filename).c_str());
        if (!tmp_backing.empty()) {
            g_iv.kfn_chunk_backing = Mix_LoadWAV(tmp_backing.c_str());
            if (g_iv.kfn_chunk_backing) {
                g_iv.kfn_tmp_backing = tmp_backing;
                g_iv.kfn_channel_backing = Mix_PlayChannel(-1, g_iv.kfn_chunk_backing, 0);
                if (g_iv.kfn_channel_backing >= 0)
                    Mix_Volume(g_iv.kfn_channel_backing, (int)(g_iv.volume * 128.0f));
            } else {
                iv_delete_tempfile(tmp_backing.c_str());
            }
        }
    }

    g_iv.kfn_active       = true;
    g_iv.kfn_current_word = 0;
    g_iv.error[0] = '\0';
    return true;
}

// ============================================================================
// TICK  (call from iv_tick every frame while g_iv.kfn_active)
// ============================================================================

void iv_kfn_tick() {
    if (!g_iv.kfn_active) return;

    // Advance the current word using the already-updated wall-clock
    // audio_position (iv_tick refreshes it before calling us).
    double t = g_iv.audio_position;
    int word = 0;
    for (size_t i = 0; i < g_iv.kfn_sync_times.size(); i++) {
        if (t >= g_iv.kfn_sync_times[i]) word = (int)i;
        else break;
    }
    g_iv.kfn_current_word = word;
}

// ============================================================================
// RENDER  (call from iv_render's display-area block)
// ============================================================================

void iv_kfn_render(float ix, float iy, float iw, float ih) {
    int rh = iv_row_h();
    float alpha_mul = g_iv.audio_paused ? 0.6f : 1.0f;

    char header[600];
    snprintf(header, sizeof(header), "%s \xe2\x80\x94 %s", g_iv.kfn_artist.c_str(), g_iv.kfn_title.c_str());
    float lbl_y = iy + ih * 0.10f;
    iv_draw_text(header, ix + iw * 0.5f - strlen(header) * g_font_size * 0.3f,
                 lbl_y, 0.90f, 0.95f, 1.00f, alpha_mul);
    if (g_iv.audio_paused) {
        const char *ps = "\xe2\x80\x96 PAUSED";
        iv_draw_text(ps, ix + iw * 0.5f - strlen(ps) * g_font_size * 0.3f,
                     lbl_y + rh, 1.00f, 0.75f, 0.30f, 1.f);
    }

    // Layer the audio visualizer underneath the lyrics, dimmed, the way the
    // original web player showed it running behind the karaoke text.
    float viz_top = lbl_y + rh * 1.6f;
    float viz_bot = iy + ih - (float)rh * 2.6f;
    if (viz_bot > viz_top) {
        iv_draw_visualizer(ix + iw * 0.03f, viz_top, iw * 0.94f, viz_bot - viz_top,
                            alpha_mul * (g_iv.audio_paused ? 0.18f : 0.35f));
    }

    // Locate the current lyric line from kfn_current_word.
    int lineIdx = 0, lineStart = 0;
    int nLines = (int)g_iv.kfn_line_indices.size();
    for (int i = 0; i < nLines; i++) {
        if (i == nLines - 1 || g_iv.kfn_current_word < g_iv.kfn_line_indices[i + 1]) {
            lineIdx = i;
            lineStart = g_iv.kfn_line_indices[i];
            break;
        }
    }
    int lineEnd = (lineIdx + 1 < nLines) ? g_iv.kfn_line_indices[lineIdx + 1] : (int)g_iv.kfn_lyrics.size();

    // Lyrics render well above normal UI text size so they read from
    // across the room, same as a typical karaoke screen.
    int cur_font  = (int)(g_font_size * 2.0f);
    int next_font = (int)(g_font_size * 1.25f);

    float line_y = iy + ih * 0.42f;
    if (!g_iv.kfn_lyrics.empty()) {
        // Rough centering: estimate line width from character count first.
        int totalChars = 0;
        for (int w = lineStart; w < lineEnd; w++) totalChars += (int)g_iv.kfn_lyrics[w].size() + 1;
        float x = ix + iw * 0.5f - totalChars * cur_font * 0.3f;

        for (int w = lineStart; w < lineEnd; w++) {
            float r, g, b;
            if (w == g_iv.kfn_current_word)      { r = 1.00f; g = 0.85f; b = 0.20f; } // singing now
            else if (w < g_iv.kfn_current_word)  { r = 0.55f; g = 0.85f; b = 1.00f; } // already sung
            else                                  { r = 0.75f; g = 0.75f; b = 0.85f; } // upcoming
            std::string word = g_iv.kfn_lyrics[w] + " ";
            draw_text(word.c_str(), x, line_y, cur_font, cur_font, r, g, b, alpha_mul);
            x += word.size() * cur_font * 0.6f;
        }
    } else {
        const char *msg = "(no synced lyrics in this file)";
        draw_text(msg, ix + iw * 0.5f - strlen(msg) * cur_font * 0.3f,
                  line_y, cur_font, cur_font, 0.6f, 0.6f, 0.7f, alpha_mul);
    }

    if (lineIdx + 1 < (int)g_iv.kfn_lyrics_lines.size()) {
        const std::string &next = g_iv.kfn_lyrics_lines[lineIdx + 1];
        float ny = line_y + rh * 2.0f;
        draw_text(next.c_str(), ix + iw * 0.5f - next.size() * next_font * 0.28f,
                  ny, next_font, next_font, 0.45f, 0.50f, 0.60f, alpha_mul * 0.85f);
    }

    // Progress bar (mirrors the plain-audio layout in iv_render)
    double total = g_iv.music ? Mix_MusicDuration(g_iv.music) : 0.0;
    float  prog  = (total > 0) ? (float)(g_iv.audio_position / total) : 0.f;
    if (prog > 1.f) prog = 1.f;
    float bar_y = iy + ih - (float)rh * 2.0f;
    float bar_x = ix + iw * 0.05f;
    float bar_w = iw * 0.90f;
    draw_rect(bar_x, bar_y, bar_w, 6.f, 0.15f, 0.15f, 0.20f, 1.f);
    if (prog > 0.f) draw_rect(bar_x, bar_y, bar_w * prog, 6.f, 0.30f, 0.80f, 0.40f, 1.f);

    int cur_s = (int)g_iv.audio_position, tot_s = (int)total;
    char timebuf[32];
    snprintf(timebuf, sizeof(timebuf), "%d:%02d / %d:%02d", cur_s / 60, cur_s % 60, tot_s / 60, tot_s % 60);
    iv_draw_text(timebuf, bar_x + bar_w * 0.5f - strlen(timebuf) * g_font_size * 0.3f,
                 bar_y + 14.f, 0.55f, 0.65f, 0.75f, 1.f);

    char status[128];
    snprintf(status, sizeof(status), "Vocal: %s   Backing: %s   Vol: %d%%",
             g_iv.kfn_vocal_muted ? "muted" : "on",
             (g_iv.kfn_channel_backing < 0) ? "n/a" : (g_iv.kfn_backing_muted ? "muted" : "on"),
             (int)(g_iv.volume * 100.f));
    iv_draw_text(status, bar_x, bar_y + 14.f, 0.50f, 0.70f, 0.85f, 1.f);

    const char *ctrl = "Space: pause/resume   S: stop   K: mute vocal   B: mute backing   +/-: vol   .: repeat";
    iv_draw_text(ctrl, ix + iw * 0.5f - strlen(ctrl) * g_font_size * 0.3f,
                 bar_y + (float)rh + 4.f, 0.40f, 0.45f, 0.55f, 1.f);
}

// ============================================================================
// MUTE TOGGLES  (bound to K / B in fc_input.cpp)
// ============================================================================

void iv_kfn_toggle_vocal_mute() {
    if (!g_iv.kfn_active) return;
    g_iv.kfn_vocal_muted = !g_iv.kfn_vocal_muted;
    int vol = g_iv.kfn_vocal_muted ? 0 : (int)(g_iv.volume * 128.0f);
    if (g_iv.music)                     Mix_VolumeMusic(vol);
    else if (g_iv.chunk_channel >= 0)   Mix_Volume(g_iv.chunk_channel, vol);
}

void iv_kfn_toggle_backing_mute() {
    if (!g_iv.kfn_active || g_iv.kfn_channel_backing < 0) return;
    g_iv.kfn_backing_muted = !g_iv.kfn_backing_muted;
    Mix_Volume(g_iv.kfn_channel_backing, g_iv.kfn_backing_muted ? 0 : (int)(g_iv.volume * 128.0f));
}
