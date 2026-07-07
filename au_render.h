#pragma once

#include <vector>
#include <cstdint>

bool render_au_to_wav(const std::vector<uint8_t> &au_bytes, 
                      std::vector<uint8_t> &wav_bytes);
