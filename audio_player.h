#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <SDL2/SDL.h>
#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include <ctime>
#include <sys/types.h>

// Conversion Cache Entry
typedef struct {
    char *original_path;
    char *virtual_filename;
    time_t modification_time;
    off_t file_size;
} ConversionCacheEntry;

// Conversion Cache
typedef struct {
    ConversionCacheEntry *entries;
    int count;
    int capacity;
} ConversionCache;

// Audio Buffer
typedef struct {
    int16_t *data;
    size_t length;
    size_t position;
} AudioBuffer;

// Play Queue
typedef struct {
    char **files;
    int count;
    int capacity;
    int current_index;
} PlayQueue;

// Main Audio Player
typedef struct {
    char current_file[1024];
    char temp_wav_file[1024];
    double song_duration;
    
    int sample_rate;
    int channels;
    int bits_per_sample;
    double playback_speed;
    
    AudioBuffer audio_buffer;
    SDL_AudioDeviceID audio_device;
    SDL_AudioSpec audio_spec;
    
    PlayQueue queue;
    ConversionCache conversion_cache;
} AudioPlayer;

// Caching functions
void init_conversion_cache(ConversionCache *cache);
void cleanup_conversion_cache(ConversionCache *cache);
const char* get_cached_conversion(ConversionCache *cache, const char* original_path);
void add_to_conversion_cache(ConversionCache *cache, const char* original_path, const char* virtual_filename);
bool convertM4aToWavInMemory(const std::vector<unsigned char>& m4a_data, 
                              std::vector<unsigned char>& wav_data);
bool convertWmaToWavInMemory(const std::vector<unsigned char>& wma_data, 
                              std::vector<unsigned char>& wav_data);
#ifdef __linux__
bool convertAudioToWavInMemory(const std::vector<unsigned char>& audio_data, std::vector<unsigned char>& wav_data, const char* file_extension);
#endif

#ifdef _WIN32
bool convertAudioToWavInMemory(const std::vector<uint8_t>& audio_data, std::vector<uint8_t>& wav_data, const char* file_extension);
#endif

#endif // AUDIO_PLAYER_H
