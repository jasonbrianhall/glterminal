#include "aiff_render.h"
#include <cstring>

struct WAVHeader {
    char riff[4];
    uint32_t size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
};

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t read_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static double read_80bit_float(const uint8_t *p) {
    // 80-bit IEEE 754 extended precision
    // Sign bit, 15-bit exponent, 64-bit mantissa
    uint16_t se = ((uint16_t)p[0] << 8) | p[1];
    int sign = (se >> 15) & 1;
    int exponent = (se & 0x7FFF) - 16383;
    
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; i++) {
        mantissa = (mantissa << 8) | p[2 + i];
    }
    
    double value = (double)mantissa / (1ULL << 63);
    
    // Apply exponent
    for (int i = 0; i < exponent; i++) value *= 2.0;
    for (int i = 0; i < -exponent; i++) value /= 2.0;
    
    return sign ? -value : value;
}

bool render_aiff_to_wav(const std::vector<uint8_t> &aiff_bytes, 
                        std::vector<uint8_t> &wav_bytes) {
    if (aiff_bytes.size() < 12) {
        return false;
    }

    const uint8_t *ptr = aiff_bytes.data();
    const uint8_t *end = ptr + aiff_bytes.size();

    if (memcmp(ptr, "FORM", 4) != 0) {
        return false;
    }

    uint32_t form_size = read_be32(ptr + 4);
    
    if (memcmp(ptr + 8, "AIFF", 4) != 0 && memcmp(ptr + 8, "AIFC", 4) != 0) {
        return false;
    }

    ptr += 12;

    uint16_t channels = 0;
    uint32_t num_frames = 0;
    uint16_t bits_per_sample = 0;
    double sample_rate_f = 0;
    uint32_t sample_rate = 0;
    const uint8_t *sound_data = nullptr;
    uint32_t sound_size = 0;

    while (ptr < end) {
        if (ptr + 8 > end) break;

        char chunk_id[4];
        memcpy(chunk_id, ptr, 4);
        uint32_t chunk_size = read_be32(ptr + 4);

        ptr += 8;

        if (ptr + chunk_size > end) break;

        if (memcmp(chunk_id, "COMM", 4) == 0 && chunk_size >= 18) {
            channels = read_be16(ptr);
            num_frames = read_be32(ptr + 2);
            bits_per_sample = read_be16(ptr + 6);
            sample_rate_f = read_80bit_float(ptr + 8);
            sample_rate = (uint32_t)(sample_rate_f + 0.5);
        }
        else if (memcmp(chunk_id, "SSND", 4) == 0 && chunk_size >= 8) {
            uint32_t offset = read_be32(ptr);
            uint32_t block_align = read_be32(ptr + 4);
            sound_data = ptr + 8 + offset;
            sound_size = chunk_size - 8 - offset;
        }

        ptr += chunk_size + (chunk_size & 1);
    }

    if (!sound_data || sound_size == 0 || channels == 0 || sample_rate == 0) {
        return false;
    }

    // Convert AIFF big-endian audio to little-endian for WAV
    std::vector<uint8_t> pcm_converted(sound_size);
    if (bits_per_sample == 16) {
        for (size_t i = 0; i + 1 < sound_size; i += 2) {
            pcm_converted[i] = sound_data[i + 1];
            pcm_converted[i + 1] = sound_data[i];
        }
    } else if (bits_per_sample == 24) {
        for (size_t i = 0; i + 2 < sound_size; i += 3) {
            pcm_converted[i] = sound_data[i + 2];
            pcm_converted[i + 1] = sound_data[i + 1];
            pcm_converted[i + 2] = sound_data[i];
        }
    } else if (bits_per_sample == 32) {
        for (size_t i = 0; i + 3 < sound_size; i += 4) {
            pcm_converted[i] = sound_data[i + 3];
            pcm_converted[i + 1] = sound_data[i + 2];
            pcm_converted[i + 2] = sound_data[i + 1];
            pcm_converted[i + 3] = sound_data[i];
        }
    } else {
        // 8-bit or other — no conversion needed
        memcpy(pcm_converted.data(), sound_data, sound_size);
    }

    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);

    WAVHeader wav_hdr;
    memcpy(wav_hdr.riff, "RIFF", 4);
    memcpy(wav_hdr.wave, "WAVE", 4);
    memcpy(wav_hdr.fmt, "fmt ", 4);
    memcpy(wav_hdr.data, "data", 4);

    wav_hdr.fmt_size = 16;
    wav_hdr.audio_format = 1;
    wav_hdr.channels = channels;
    wav_hdr.sample_rate = sample_rate;
    wav_hdr.byte_rate = byte_rate;
    wav_hdr.block_align = block_align;
    wav_hdr.bits_per_sample = bits_per_sample;
    wav_hdr.data_size = sound_size;
    wav_hdr.size = 36 + sound_size;

    wav_bytes.resize(sizeof(WAVHeader) + sound_size);
    memcpy(wav_bytes.data(), &wav_hdr, sizeof(WAVHeader));
    memcpy(wav_bytes.data() + sizeof(WAVHeader), pcm_converted.data(), sound_size);

    return true;
}
