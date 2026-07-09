#pragma once

#include <vector>
#include <cstdint>

// Windows audio converter using Media Foundation
// Converts M4A, WMA, MP2, and other formats to WAV

bool convertAudioToWavInMemory(const std::vector<uint8_t>& audio_data, std::vector<uint8_t>& wav_data, const char* file_extension);
bool convertAudioToWav(const char* input_filename, const char* wav_filename);
