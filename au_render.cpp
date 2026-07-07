#include "au_render.h"
#include <cstring>

struct AUHeader {
    uint32_t magic;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t encoding;
    uint32_t sample_rate;
    uint32_t channels;
};

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

static void write_le32(uint8_t *p, uint32_t val) {
    p[0] = val & 0xFF;
    p[1] = (val >> 8) & 0xFF;
    p[2] = (val >> 16) & 0xFF;
    p[3] = (val >> 24) & 0xFF;
}

static void write_le16(uint8_t *p, uint16_t val) {
    p[0] = val & 0xFF;
    p[1] = (val >> 8) & 0xFF;
}

bool render_au_to_wav(const std::vector<uint8_t> &au_bytes, 
                      std::vector<uint8_t> &wav_bytes) {
    if (au_bytes.size() < 24) {
        return false;
    }

    const uint8_t *ptr = au_bytes.data();

    uint32_t magic = read_be32(ptr);
    if (magic != 0x2E736E64) {
        return false;
    }

    uint32_t data_offset = read_be32(ptr + 4);
    uint32_t data_size = read_be32(ptr + 8);
    uint32_t encoding = read_be32(ptr + 12);
    uint32_t sample_rate = read_be32(ptr + 16);
    uint32_t channels = read_be32(ptr + 20);

    if (data_offset < 24 || data_offset >= au_bytes.size()) {
        return false;
    }

    uint16_t bits_per_sample = 8;
    
    switch (encoding) {
        case 1: bits_per_sample = 8; break;
        case 2: bits_per_sample = 8; break;
        case 3: bits_per_sample = 16; break;
        case 4: bits_per_sample = 24; break;
        case 5: bits_per_sample = 32; break;
        default: return false;
    }

    size_t pcm_size = au_bytes.size() - data_offset;
    if (data_size != 0xFFFFFFFFU && data_size < pcm_size) {
        pcm_size = data_size;
    }

    if (pcm_size == 0 || channels == 0 || sample_rate == 0) {
        return false;
    }

    const uint8_t *pcm_data = ptr + data_offset;

    uint16_t wav_channels = (channels > 0 && channels < 256) ? channels : 1;
    uint32_t byte_rate = sample_rate * wav_channels * (bits_per_sample / 8);
    uint16_t block_align = wav_channels * (bits_per_sample / 8);

    // Convert AU big-endian audio to little-endian for WAV
    std::vector<uint8_t> pcm_converted(pcm_size);
    if (bits_per_sample == 16) {
        for (size_t i = 0; i + 1 < pcm_size; i += 2) {
            pcm_converted[i] = pcm_data[i + 1];
            pcm_converted[i + 1] = pcm_data[i];
        }
    } else if (bits_per_sample == 24) {
        for (size_t i = 0; i + 2 < pcm_size; i += 3) {
            pcm_converted[i] = pcm_data[i + 2];
            pcm_converted[i + 1] = pcm_data[i + 1];
            pcm_converted[i + 2] = pcm_data[i];
        }
    } else if (bits_per_sample == 32) {
        for (size_t i = 0; i + 3 < pcm_size; i += 4) {
            pcm_converted[i] = pcm_data[i + 3];
            pcm_converted[i + 1] = pcm_data[i + 2];
            pcm_converted[i + 2] = pcm_data[i + 1];
            pcm_converted[i + 3] = pcm_data[i];
        }
    } else {
        // 8-bit or other — no conversion needed
        memcpy(pcm_converted.data(), pcm_data, pcm_size);
    }

    WAVHeader wav_hdr;
    memcpy(wav_hdr.riff, "RIFF", 4);
    memcpy(wav_hdr.wave, "WAVE", 4);
    memcpy(wav_hdr.fmt, "fmt ", 4);
    memcpy(wav_hdr.data, "data", 4);

    wav_hdr.fmt_size = 16;
    wav_hdr.audio_format = 1;
    wav_hdr.channels = wav_channels;
    wav_hdr.sample_rate = sample_rate;
    wav_hdr.byte_rate = byte_rate;
    wav_hdr.block_align = block_align;
    wav_hdr.bits_per_sample = bits_per_sample;
    wav_hdr.data_size = pcm_size;
    wav_hdr.size = 36 + pcm_size;

    wav_bytes.resize(sizeof(WAVHeader) + pcm_size);
    memcpy(wav_bytes.data(), &wav_hdr, sizeof(WAVHeader));
    memcpy(wav_bytes.data() + sizeof(WAVHeader), pcm_converted.data(), pcm_size);

    return true;
}
