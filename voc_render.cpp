#include "voc_render.h"
#include <cstring>
#include <cstdio>

// VOC block types
#define VOC_BLOCK_TERMINATOR        0
#define VOC_BLOCK_SOUND_DATA        1
#define VOC_BLOCK_SOUND_CONT        2
#define VOC_BLOCK_SILENCE           3
#define VOC_BLOCK_MARKER            4
#define VOC_BLOCK_TEXT              5
#define VOC_BLOCK_REPEAT_START      6
#define VOC_BLOCK_REPEAT_END        7
#define VOC_BLOCK_EXTENDED          8
#define VOC_BLOCK_SOUND_DATA_NEW    9

struct VOCHeader {
    char signature[20];  // "Creative Voice File"
    uint8_t terminator;
    uint16_t version;
    uint16_t header_size;
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

static uint32_t read_uint24_le(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16);
}

bool render_voc_to_wav(const std::vector<uint8_t> &voc_bytes, 
                       std::vector<uint8_t> &wav_bytes) {
    if (voc_bytes.size() < 26) {
        return false;
    }

    const uint8_t *ptr = voc_bytes.data();
    const uint8_t *end = ptr + voc_bytes.size();

    if (memcmp(ptr, "Creative Voice File\x1A", 20) != 0) {
        return false;
    }

    uint16_t voc_version = (ptr[22] | (ptr[23] << 8));

    uint32_t data_block_offset = 26;
    
    if (voc_version < 0x0114) {
        data_block_offset = (ptr[24] | (ptr[25] << 8));
    }

    ptr = voc_bytes.data() + data_block_offset;

    std::vector<uint8_t> pcm_data;
    uint32_t sample_rate = 11025;
    uint16_t bits_per_sample = 8;
    uint16_t channels = 1;
    bool has_explicit_rate = false;
    uint32_t total_audio_bytes = 0;

    while (ptr < end) {
        if (ptr + 4 > end) break;

        uint8_t block_type = ptr[0];
        uint32_t block_size = read_uint24_le(ptr + 1);


        ptr += 4;

        if (ptr + block_size > end) break;

        if (block_type == VOC_BLOCK_TERMINATOR) {
            break;
        }
        else if (block_type == VOC_BLOCK_SOUND_DATA) {
            if (block_size < 2) {
                ptr += block_size;
                continue;
            }

            uint8_t codec_byte = ptr[0];
            uint8_t channels_byte = ptr[1];


            // Even if codec is not 0, extract the sample rate for following SOUND_CONT blocks
            if (codec_byte == 0) {
                // Uncompressed PCM
                if (!has_explicit_rate) {
                    sample_rate = 11025;
                }
                channels = channels_byte + 1;
                bits_per_sample = 8;

                size_t audio_size = block_size - 2;
                total_audio_bytes += audio_size;
                pcm_data.insert(pcm_data.end(), ptr + 2, ptr + 2 + audio_size);
            } else {
                // Compressed codec - extract rate info for upcoming SOUND_CONT blocks
                // codec_byte encodes: rate = 1000000 / (256 - codec_byte)
                sample_rate = 1000000 / (256 - codec_byte);
                channels = channels_byte + 1;
                bits_per_sample = 8;
            }
            ptr += block_size;
            continue;
        }
        else if (block_type == VOC_BLOCK_SOUND_CONT) {
            total_audio_bytes += block_size;
            pcm_data.insert(pcm_data.end(), ptr, ptr + block_size);
        }
        else if (block_type == VOC_BLOCK_SOUND_DATA_NEW) {
            if (block_size < 12) {
                ptr += block_size;
                continue;
            }

            sample_rate = (ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24));
            bits_per_sample = ptr[4];
            channels = ptr[5];
            uint16_t compression_type = (ptr[6] | (ptr[7] << 8));
            has_explicit_rate = true;

            if (compression_type != 0) {
                ptr += block_size;
                continue;
            }

            size_t audio_size = block_size - 12;
            total_audio_bytes += audio_size;
            pcm_data.insert(pcm_data.end(), ptr + 12, ptr + 12 + audio_size);
        }

        ptr += block_size;
    }

    if (pcm_data.empty()) {
        return false;
    }

    // If we never found an explicit sample rate, try to guess from file size
    if (!has_explicit_rate && pcm_data.size() > 0) {
        if (sample_rate < 8000) {
            uint32_t guessed_rates[] = {22050, 44100, 48000, 16000, 11025};
            for (uint32_t rate : guessed_rates) {
                double duration = (double)pcm_data.size() / (rate * channels * (bits_per_sample / 8));
                if (duration > 30 && duration < 600) {
                    sample_rate = rate;
                    break;
                }
            }
        }
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
    wav_hdr.data_size = pcm_data.size();
    wav_hdr.size = 36 + pcm_data.size();

    wav_bytes.resize(sizeof(WAVHeader) + pcm_data.size());
    memcpy(wav_bytes.data(), &wav_hdr, sizeof(WAVHeader));
    memcpy(wav_bytes.data() + sizeof(WAVHeader), pcm_data.data(), pcm_data.size());

    return true;
}
