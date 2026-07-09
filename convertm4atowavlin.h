#pragma once

#include <vector>
#include <cstdint>

// Linux audio converter using ffmpeg/libav
// Converts M4A, WMA, MP2, and other formats to WAV

bool convertM4aToWavInMemory(const std::vector<uint8_t>& m4a_data, std::vector<uint8_t>& wav_data);
bool convertM4aToWav(const char* m4a_filename, const char* wav_filename);
