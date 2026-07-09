#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include "vfs.h"

// Minimal stubs for webserver build (doesn't use VFS, only audio conversion)
// The full VFS implementation is used by the AudioPlayer desktop app

void init_virtual_filesystem() {}
void cleanup_virtual_filesystem() {}

VirtualFile* create_virtual_file(const char* filename) {
    return nullptr;
}

VirtualFile* get_virtual_file(const char* filename) {
    return nullptr;
}

bool delete_virtual_file(const char* filename) {
    return false;
}

void free_virtual_file(void* data) {
    if (data) free(data);
}

bool virtual_file_write(VirtualFile* vf, const void* data, size_t size) {
    return false;
}

size_t virtual_file_read(VirtualFile* vf, void* buffer, size_t size) {
    return 0;
}

bool virtual_file_seek(VirtualFile* vf, long offset, int whence) {
    return false;
}

long virtual_file_tell(VirtualFile* vf) {
    return -1;
}

size_t virtual_file_size(VirtualFile* vf) {
    return 0;
}

VirtualWAVConverter* virtual_wav_converter_init(const char* filename, int sample_rate, int channels) {
    return nullptr;
}

bool virtual_wav_converter_write(VirtualWAVConverter* converter, int16_t* samples, size_t count) {
    return false;
}

void virtual_wav_converter_finish(VirtualWAVConverter* converter) {}

void virtual_wav_converter_free(VirtualWAVConverter* converter) {
    if (converter) free(converter);
}

bool load_virtual_wav_file(AudioPlayer *player, const char* virtual_filename) {
    return false;
}

// Cache stubs (defined in audio_player.h but needed here)
void init_conversion_cache(ConversionCache *cache) {
    if (cache) {
        cache->entries = nullptr;
        cache->count = 0;
        cache->capacity = 0;
    }
}

void cleanup_conversion_cache(ConversionCache *cache) {
    if (cache && cache->entries) {
        for (int i = 0; i < cache->count; i++) {
            free(cache->entries[i].original_path);
            free(cache->entries[i].virtual_filename);
        }
        free(cache->entries);
        cache->entries = nullptr;
        cache->count = 0;
        cache->capacity = 0;
    }
}

const char* get_cached_conversion(ConversionCache *cache, const char* original_path) {
    if (!cache || !original_path) return nullptr;
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].original_path && strcmp(cache->entries[i].original_path, original_path) == 0) {
            return cache->entries[i].virtual_filename;
        }
    }
    return nullptr;
}

void add_to_conversion_cache(ConversionCache *cache, const char* original_path, const char* virtual_filename) {
    if (!cache || !original_path || !virtual_filename) return;
    
    // Expand if needed
    if (cache->count >= cache->capacity) {
        cache->capacity = (cache->capacity == 0) ? 10 : cache->capacity * 2;
        cache->entries = (ConversionCacheEntry*)realloc(cache->entries, cache->capacity * sizeof(ConversionCacheEntry));
    }
    
    cache->entries[cache->count].original_path = (char*)malloc(strlen(original_path) + 1);
    cache->entries[cache->count].virtual_filename = (char*)malloc(strlen(virtual_filename) + 1);
    strcpy(cache->entries[cache->count].original_path, original_path);
    strcpy(cache->entries[cache->count].virtual_filename, virtual_filename);
    cache->count++;
}
