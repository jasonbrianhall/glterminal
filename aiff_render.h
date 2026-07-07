#pragma once

#include <vector>
#include <cstdint>

bool render_aiff_to_wav(const std::vector<uint8_t> &aiff_bytes, 
                        std::vector<uint8_t> &wav_bytes);
