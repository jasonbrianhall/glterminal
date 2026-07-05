#ifndef WAV_CONVERTER_H
#define WAV_CONVERTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// WAV file header structure
typedef struct {
    // RIFF chunk descriptor
    char riff_header[4];      // "RIFF"
    uint32_t wav_size;        // File size - 8
    char wave_header[4];      // "WAVE"
    
    // fmt subchunk
    char fmt_header[4];       // "fmt "
    uint32_t fmt_chunk_size;  // 16 for PCM
    uint16_t audio_format;    // 1 for PCM
    uint16_t num_channels;    // 1 or 2
    uint32_t sample_rate;     // e.g., 44100
    uint32_t byte_rate;       // sample_rate * num_channels * bytes_per_sample
    uint16_t block_align;     // num_channels * bytes_per_sample
    uint16_t bits_per_sample; // 16 for 16-bit audio
    
    // data subchunk
    char data_header[4];      // "data"
    uint32_t data_size;       // Number of bytes in data
} WAVHeader;

// Converter structure
typedef struct {
    FILE* output_file;        // Output WAV file
    WAVHeader header;         // WAV file header
    size_t data_start_pos;    // Position of data chunk in file
    size_t total_samples;     // Total samples written
    bool header_written;      // Has header been written
} WAVConverter;

// Initialize WAV converter
WAVConverter* wav_converter_init(const char* filename, 
                                 uint32_t sample_rate, 
                                 uint16_t num_channels);

// Write audio samples to WAV file
bool wav_converter_write(WAVConverter* converter, 
                         const int16_t* samples, 
                         size_t num_samples);

// Finalize and close WAV file
bool wav_converter_finish(WAVConverter* converter);

// Free converter resources
void wav_converter_free(WAVConverter* converter);

#endif // WAV_CONVERTER_H
