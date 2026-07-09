#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <cstdint>
#include <memory>
#include "audio_player.h"
#include "vfs.h"

#ifdef __linux__
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

// FFmpeg initialization helper
static bool g_ffmpeg_initialized = false;

static bool initializeFFmpeg() {
    if (g_ffmpeg_initialized) return true;
    
    // Register all formats and codecs (for older FFmpeg versions)
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
    avcodec_register_all();
#endif
    
    g_ffmpeg_initialized = true;
    return true;
}

// RAII wrappers for FFmpeg resources
struct AVFormatContextDeleter {
    void operator()(AVFormatContext* ctx) {
        if (ctx) avformat_close_input(&ctx);
    }
};

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* ctx) {
        if (ctx) avcodec_free_context(&ctx);
    }
};

struct AVIOContextDeleter {
    void operator()(AVIOContext* ctx) {
        if (ctx) avio_context_free(&ctx);
    }
};

struct SwrContextDeleter {
    void operator()(SwrContext* ctx) {
        if (ctx) swr_free(&ctx);
    }
};

struct AVPacketDeleter {
    void operator()(AVPacket* pkt) {
        if (pkt) av_packet_free(&pkt);
    }
};

struct AVFrameDeleter {
    void operator()(AVFrame* frame) {
        if (frame) av_frame_free(&frame);
    }
};

struct AVSamplesDeleter {
    uint8_t** data;
    AVSamplesDeleter(uint8_t** d) : data(d) {}
    ~AVSamplesDeleter() {
        if (data) {
            if (data[0]) av_freep(&data[0]);
            av_freep(&data);
        }
    }
};

using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using AVIOContextPtr = std::unique_ptr<AVIOContext, AVIOContextDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

// Memory-based I/O context for FFmpeg
struct MemoryIOContext {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

static int memory_read(void* opaque, uint8_t* buf, int buf_size) {
    MemoryIOContext* ctx = (MemoryIOContext*)opaque;
    int bytes_to_read = buf_size;
    
    if (ctx->pos >= ctx->size) {
        return AVERROR_EOF;
    }
    
    if (ctx->pos + bytes_to_read > ctx->size) {
        bytes_to_read = ctx->size - ctx->pos;
    }
    
    if (bytes_to_read > 0) {
        memcpy(buf, ctx->data + ctx->pos, bytes_to_read);
        ctx->pos += bytes_to_read;
    }
    
    return bytes_to_read;
}

static int64_t memory_seek(void* opaque, int64_t offset, int whence) {
    MemoryIOContext* ctx = (MemoryIOContext*)opaque;
    
    switch (whence) {
        case SEEK_SET:
            ctx->pos = offset;
            break;
        case SEEK_CUR:
            ctx->pos += offset;
            break;
        case SEEK_END:
            ctx->pos = ctx->size + offset;
            break;
        case AVSEEK_SIZE:
            return ctx->size;
        default:
            return -1;
    }
    
    if (ctx->pos > ctx->size) {
        ctx->pos = ctx->size;
    }
    
    return ctx->pos;
}

bool convertM4aToWavInMemory(const std::vector<uint8_t>& m4a_data, std::vector<uint8_t>& wav_data) {
    if (!initializeFFmpeg()) {
        return false;
    }
    
    // Set up memory I/O context
    MemoryIOContext mem_ctx;
    mem_ctx.data = m4a_data.data();
    mem_ctx.size = m4a_data.size();
    mem_ctx.pos = 0;
    
    const int avio_buffer_size = 4096;
    uint8_t* avio_buffer = (uint8_t*)av_malloc(avio_buffer_size);
    if (!avio_buffer) {
        printf("Failed to allocate AVIO buffer\n");
        return false;
    }
    
    AVIOContextPtr avio_ctx(avio_alloc_context(avio_buffer, avio_buffer_size, 0, &mem_ctx, 
                                              memory_read, nullptr, memory_seek));
    if (!avio_ctx) {
        printf("Failed to create AVIO context\n");
        av_free(avio_buffer);
        return false;
    }
    
    // Allocate format context
    AVFormatContext* format_ctx_raw = avformat_alloc_context();
    if (!format_ctx_raw) {
        printf("Failed to allocate format context\n");
        return false;
    }
    
    format_ctx_raw->pb = avio_ctx.get();
    AVFormatContextPtr format_ctx(format_ctx_raw);
    
    // Open input
    AVFormatContext* temp_ctx = format_ctx.release();
    if (avformat_open_input(&temp_ctx, nullptr, nullptr, nullptr) < 0) {
        printf("Failed to open M4A data\n");
        if (temp_ctx) avformat_close_input(&temp_ctx);
        return false;
    }
    format_ctx.reset(temp_ctx);
    
    // Find stream info
    if (avformat_find_stream_info(format_ctx.get(), nullptr) < 0) {
        printf("Failed to find stream info\n");
        return false;
    }
    
    // Find audio stream
    int audio_stream_index = -1;
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }
    
    if (audio_stream_index == -1) {
        printf("No audio stream found\n");
        return false;
    }
    
    AVStream* audio_stream = format_ctx->streams[audio_stream_index];
    AVCodecParameters* codecpar = audio_stream->codecpar;
    
    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        printf("Codec not found\n");
        return false;
    }
    
    // Create codec context
    AVCodecContextPtr codec_ctx(avcodec_alloc_context3(codec));
    if (!codec_ctx) {
        printf("Failed to allocate codec context\n");
        return false;
    }
    
    // Copy codec parameters
    if (avcodec_parameters_to_context(codec_ctx.get(), codecpar) < 0) {
        printf("Failed to copy codec parameters\n");
        return false;
    }
    
    // Open codec
    if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
        printf("Failed to open codec\n");
        return false;
    }
    
    // Get channel count (compatibility with different FFmpeg versions)
    int channels;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
    channels = codec_ctx->ch_layout.nb_channels;
#else
    channels = codec_ctx->channels;
#endif
    
    printf("M4A (memory): %d Hz, %d channels, %d bits per sample\n", 
           codec_ctx->sample_rate, channels, 
           av_get_bytes_per_sample(codec_ctx->sample_fmt) * 8);
    
    // Set up resampler for conversion to 16-bit PCM
    SwrContextPtr swr_ctx(swr_alloc());
    if (!swr_ctx) {
        printf("Failed to allocate resampler\n");
        return false;
    }
    
    // Configure resampler
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
    AVChannelLayout in_ch_layout, out_ch_layout;
    av_channel_layout_copy(&in_ch_layout, &codec_ctx->ch_layout);
    av_channel_layout_default(&out_ch_layout, channels);
    
    av_opt_set_chlayout(swr_ctx.get(), "in_chlayout", &in_ch_layout, 0);
    av_opt_set_chlayout(swr_ctx.get(), "out_chlayout", &out_ch_layout, 0);
    
    av_channel_layout_uninit(&in_ch_layout);
    av_channel_layout_uninit(&out_ch_layout);
#else
    uint64_t in_layout = codec_ctx->channel_layout ? codec_ctx->channel_layout : av_get_default_channel_layout(channels);
    av_opt_set_int(swr_ctx.get(), "in_channel_layout", in_layout, 0);
    av_opt_set_int(swr_ctx.get(), "out_channel_layout", av_get_default_channel_layout(channels), 0);
#endif
    
    av_opt_set_int(swr_ctx.get(), "in_sample_rate", codec_ctx->sample_rate, 0);
    av_opt_set_int(swr_ctx.get(), "out_sample_rate", codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx.get(), "in_sample_fmt", codec_ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr_ctx.get(), "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    
    if (swr_init(swr_ctx.get()) < 0) {
        printf("Failed to initialize resampler\n");
        return false;
    }
    
    // Prepare WAV data vector
    wav_data.clear();
    wav_data.reserve(m4a_data.size() * 2); // Rough estimate
    
    // Helper function to append data to vector
    auto append_bytes = [&wav_data](const void* data, size_t size) {
        const uint8_t* bytes = (const uint8_t*)data;
        wav_data.insert(wav_data.end(), bytes, bytes + size);
    };
    
    // Write WAV header (we'll update the sizes later)
    size_t riff_size_pos, data_size_pos;
    
    append_bytes("RIFF", 4);
    riff_size_pos = wav_data.size();
    uint32_t placeholder = 0;
    append_bytes(&placeholder, 4); // File size (will update later)
    append_bytes("WAVE", 4);
    
    // fmt chunk
    append_bytes("fmt ", 4);
    int fmt_chunk_size = 16;
    short audio_format = 1; // PCM
    short wav_channels = (short)channels;
    int sample_rate = codec_ctx->sample_rate;
    short bits_per_sample = 16;
    int byte_rate = sample_rate * channels * bits_per_sample / 8;
    short block_align = channels * bits_per_sample / 8;
    
    append_bytes(&fmt_chunk_size, 4);
    append_bytes(&audio_format, 2);
    append_bytes(&wav_channels, 2);
    append_bytes(&sample_rate, 4);
    append_bytes(&byte_rate, 4);
    append_bytes(&block_align, 2);
    append_bytes(&bits_per_sample, 2);
    
    // data chunk header
    append_bytes("data", 4);
    data_size_pos = wav_data.size();
    append_bytes(&placeholder, 4); // Data size (will update later)
    
    size_t audio_data_start = wav_data.size();
    
    // Allocate packet and frame
    AVPacketPtr packet(av_packet_alloc());
    AVFramePtr frame(av_frame_alloc());
    
    if (!packet || !frame) {
        printf("Failed to allocate packet or frame\n");
        return false;
    }
    
    // Allocate output buffer for resampler
    uint8_t** output_buffer = nullptr;
    int output_linesize = 0;
    int max_samples = 1024; // Conservative estimate
    
    int ret = av_samples_alloc_array_and_samples(&output_buffer, &output_linesize,
                                               channels, max_samples, AV_SAMPLE_FMT_S16, 0);
    if (ret < 0) {
        printf("Failed to allocate output buffer\n");
        return false;
    }
    
    // Use RAII wrapper for output buffer
    AVSamplesDeleter output_deleter(output_buffer);
    
    // Decode and convert audio data
    while (av_read_frame(format_ctx.get(), packet.get()) >= 0) {
        if (packet->stream_index == audio_stream_index) {
            if (avcodec_send_packet(codec_ctx.get(), packet.get()) >= 0) {
                while (avcodec_receive_frame(codec_ctx.get(), frame.get()) >= 0) {
                    // Reallocate output buffer if needed
                    if (frame->nb_samples > max_samples) {
                        output_deleter.~AVSamplesDeleter();
                        output_buffer = nullptr;
                        
                        max_samples = frame->nb_samples;
                        ret = av_samples_alloc_array_and_samples(&output_buffer, &output_linesize,
                                                               channels, max_samples, AV_SAMPLE_FMT_S16, 0);
                        if (ret < 0) {
                            printf("Failed to reallocate output buffer\n");
                            return false;
                        }
                        new (&output_deleter) AVSamplesDeleter(output_buffer);
                    }
                    
                    // Convert to 16-bit PCM
                    int output_samples = swr_convert(swr_ctx.get(), output_buffer, frame->nb_samples,
                                                   (const uint8_t**)frame->data, frame->nb_samples);
                    
                    if (output_samples > 0) {
                        int output_size = output_samples * channels * sizeof(int16_t);
                        append_bytes(output_buffer[0], output_size);
                    }
                }
            }
        }
        av_packet_unref(packet.get());
    }
    
    // Flush decoder
    avcodec_send_packet(codec_ctx.get(), nullptr);
    while (avcodec_receive_frame(codec_ctx.get(), frame.get()) >= 0) {
        // Reallocate output buffer if needed
        if (frame->nb_samples > max_samples) {
            output_deleter.~AVSamplesDeleter();
            output_buffer = nullptr;
            
            max_samples = frame->nb_samples;
            ret = av_samples_alloc_array_and_samples(&output_buffer, &output_linesize,
                                                   channels, max_samples, AV_SAMPLE_FMT_S16, 0);
            if (ret < 0) {
                printf("Failed to reallocate output buffer during flush\n");
                break;
            }
            new (&output_deleter) AVSamplesDeleter(output_buffer);
        }
        
        int output_samples = swr_convert(swr_ctx.get(), output_buffer, frame->nb_samples,
                                       (const uint8_t**)frame->data, frame->nb_samples);
        
        if (output_samples > 0) {
            int output_size = output_samples * channels * sizeof(int16_t);
            append_bytes(output_buffer[0], output_size);
        }
    }
    
    // Update WAV header with actual sizes
    uint32_t data_size = (uint32_t)(wav_data.size() - audio_data_start);
    uint32_t file_size = (uint32_t)(wav_data.size() - 8);
    
    // Update RIFF chunk size
    memcpy(&wav_data[riff_size_pos], &file_size, 4);
    
    // Update data chunk size
    memcpy(&wav_data[data_size_pos], &data_size, 4);
    
    printf("M4A to WAV memory conversion complete (%zu bytes)\n", wav_data.size());
    return !wav_data.empty();
}

// Similar fixes should be applied to the file-based version
bool convertM4aToWav(const char* m4a_filename, const char* wav_filename) {
    // Read the file into memory first
    FILE* file = fopen(m4a_filename, "rb");
    if (!file) {
        printf("Cannot open M4A file: %s\n", m4a_filename);
        return false;
    }
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    std::vector<uint8_t> m4a_data(size);
    if (fread(m4a_data.data(), 1, size, file) != (size_t)size) {
        fclose(file);
        printf("Failed to read M4A file\n");
        return false;
    }
    fclose(file);
    
    // Convert in memory
    std::vector<uint8_t> wav_data;
    if (!convertM4aToWavInMemory(m4a_data, wav_data)) {
        return false;
    }
    
    // Write to file
    FILE* wav_file = fopen(wav_filename, "wb");
    if (!wav_file) {
        printf("Cannot create WAV file: %s\n", wav_filename);
        return false;
    }
    
    size_t written = fwrite(wav_data.data(), 1, wav_data.size(), wav_file);
    fclose(wav_file);
    
    if (written != wav_data.size()) {
        printf("Failed to write complete WAV file\n");
        return false;
    }
    
    printf("M4A conversion complete\n");
    return true;
}

bool convert_m4a_to_wav(AudioPlayer *player, const char* filename) {
    // M4A is handled by the generic ffmpeg converter
    if (!filename) return false;
    
    // Read file into memory
    FILE* f = fopen(filename, "rb");
    if (!f) return false;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    std::vector<uint8_t> audio_data(size);
    if (fread(audio_data.data(), 1, size, f) != (size_t)size) {
        fclose(f);
        return false;
    }
    fclose(f);
    
    // Convert to WAV in memory
    std::vector<uint8_t> wav_data;
    if (!convertM4aToWavInMemory(audio_data, wav_data)) {
        return false;
    }
    
    // Store in temp WAV file
    snprintf(player->temp_wav_file, sizeof(player->temp_wav_file), "/tmp/convert_%d.wav", rand());
    FILE* wav_file = fopen(player->temp_wav_file, "wb");
    if (!wav_file) return false;
    
    if (fwrite(wav_data.data(), 1, wav_data.size(), wav_file) != wav_data.size()) {
        fclose(wav_file);
        return false;
    }
    fclose(wav_file);
    
    return true;
}

bool convert_wma_to_wav(AudioPlayer *player, const char* filename) {
    // WMA is handled by the generic ffmpeg converter
    if (!filename) return false;
    
    // Read file into memory
    FILE* f = fopen(filename, "rb");
    if (!f) return false;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    std::vector<uint8_t> audio_data(size);
    if (fread(audio_data.data(), 1, size, f) != (size_t)size) {
        fclose(f);
        return false;
    }
    fclose(f);
    
    // Convert to WAV in memory
    std::vector<uint8_t> wav_data;
    if (!convertM4aToWavInMemory(audio_data, wav_data)) {  // Works for any format
        return false;
    }
    
    // Store in temp WAV file
    snprintf(player->temp_wav_file, sizeof(player->temp_wav_file), "/tmp/convert_%d.wav", rand());
    FILE* wav_file = fopen(player->temp_wav_file, "wb");
    if (!wav_file) return false;
    
    if (fwrite(wav_data.data(), 1, wav_data.size(), wav_file) != wav_data.size()) {
        fclose(wav_file);
        return false;
    }
    fclose(wav_file);
    
    return true;
}


bool convert_audio_to_wav(AudioPlayer *player, const char* filename) {
    // Check cache first
    const char* cached_file = get_cached_conversion(&player->conversion_cache, filename);
    if (cached_file) {
        strncpy(player->temp_wav_file, cached_file, sizeof(player->temp_wav_file) - 1);
        player->temp_wav_file[sizeof(player->temp_wav_file) - 1] = '\0';
        return true;
    }
    
    // Generate a unique virtual filename
    static int virtual_counter = 0;
    char virtual_filename[256];
    snprintf(virtual_filename, sizeof(virtual_filename), "virtual_m4a_%d.wav", virtual_counter++);
    
    strncpy(player->temp_wav_file, virtual_filename, sizeof(player->temp_wav_file) - 1);
    player->temp_wav_file[sizeof(player->temp_wav_file) - 1] = '\0';
    
    printf("Converting M4A to virtual WAV: %s -> %s\n", filename, virtual_filename);
    
    // Read M4A file into memory
    FILE* m4a_file = fopen(filename, "rb");
    if (!m4a_file) {
        printf("Cannot open file: %s\n", filename);
        return false;
    }
    
    fseek(m4a_file, 0, SEEK_END);
    long m4a_size = ftell(m4a_file);
    fseek(m4a_file, 0, SEEK_SET);
    
    std::vector<uint8_t> m4a_data(m4a_size);
    if (fread(m4a_data.data(), 1, m4a_size, m4a_file) != (size_t)m4a_size) {
        printf("Failed to read M4A file\n");
        fclose(m4a_file);
        return false;
    }
    fclose(m4a_file);
    
    // Convert M4A to WAV in memory
    std::vector<uint8_t> wav_data;
    if (!convertM4aToWavInMemory(m4a_data, wav_data)) {
        printf("Conversion to WAV conversion failed\n");
        return false;
    }
    
    // Create virtual file and write WAV data
    VirtualFile* vf = create_virtual_file(virtual_filename);
    if (!vf) {
        printf("Cannot create virtual WAV file: %s\n", virtual_filename);
        return false;
    }
    
    if (!virtual_file_write(vf, wav_data.data(), wav_data.size())) {
        printf("Failed to write virtual WAV file\n");
        return false;
    }
    
    // Add to cache after successful conversion
    add_to_conversion_cache(&player->conversion_cache, filename, virtual_filename);
    
    printf("Conversion to virtual file complete\n");
    return true;
}

#endif // __linux__
