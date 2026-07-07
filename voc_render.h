#pragma once

#include <vector>
#include <cstdint>

// Convert VOC file bytes to WAV file bytes
// Returns true on success, false on parse error
bool render_voc_to_wav(const std::vector<uint8_t> &voc_bytes, 
                       std::vector<uint8_t> &wav_bytes);
