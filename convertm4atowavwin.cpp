#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <cstdint>
#include "audio_player.h"
#include "vfs.h"

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

// Media Foundation initialization helper
static bool g_mf_initialized = false;

static bool initializeMediaFoundation() {
    if (g_mf_initialized) return true;
    
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        printf("Failed to initialize COM\n");
        return false;
    }
    
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        printf("Failed to initialize Media Foundation\n");
        CoUninitialize();
        return false;
    }
    
    g_mf_initialized = true;
    return true;
}

static void cleanupMediaFoundation() {
    if (g_mf_initialized) {
        MFShutdown();
        CoUninitialize();
        g_mf_initialized = false;
    }
}

// Utility function to convert string to wstring
static std::wstring stringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &result[0], size);
    return result;
}

// Function to detect audio file format
static bool isWmaFile(const char* filename) {
    const char* ext = strrchr(filename, '.');
    return ext && (_stricmp(ext, ".wma") == 0);
}

static bool isM4aFile(const char* filename) {
    const char* ext = strrchr(filename, '.');
    return ext && (_stricmp(ext, ".m4a") == 0 || _stricmp(ext, ".m4p") == 0);
}

// Generic function that works for both M4A and WMA
static bool convertAudioToWav(const char* input_filename, const char* wav_filename) {
    if (!initializeMediaFoundation()) {
        return false;
    }
    
    // Convert filename to wide string
    std::wstring wide_filename = stringToWString(input_filename);
    
    HRESULT hr;
    IMFSourceReader* pReader = nullptr;
    
    // Create source reader from file - works for both M4A and WMA
    hr = MFCreateSourceReaderFromURL(wide_filename.c_str(), nullptr, &pReader);
    if (FAILED(hr)) {
        printf("Cannot open audio file: %s (Error: 0x%lx)\n", input_filename, hr);
        return false;
    }
    
    // GET ORIGINAL FORMAT INFORMATION FIRST
    IMFMediaType* pOriginalType = nullptr;
    hr = pReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &pOriginalType);
    if (FAILED(hr)) {
        printf("Failed to get original media type\n");
        pReader->Release();
        return false;
    }
    
    // Extract original audio parameters
    UINT32 originalSampleRate = 44100; // default fallback
    UINT32 originalChannels = 2;       // default fallback
    
    pOriginalType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &originalSampleRate);
    pOriginalType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &originalChannels);
    
    // Determine file type for logging
    const char* fileType = isWmaFile(input_filename) ? "WMA" : 
                          isM4aFile(input_filename) ? "M4A" : "Audio";
    printf("Original %s: %d Hz, %d channels\n", fileType, originalSampleRate, originalChannels);
    
    // Configure source reader for PCM output WITH ORIGINAL SAMPLE RATE
    IMFMediaType* pType = nullptr;
    hr = MFCreateMediaType(&pType);
    if (FAILED(hr)) {
        printf("Failed to create media type\n");
        pOriginalType->Release();
        pReader->Release();
        return false;
    }
    
    hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) {
        hr = pType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    }
    if (SUCCEEDED(hr)) {
        hr = pType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, originalSampleRate);
    }
    if (SUCCEEDED(hr)) {
        hr = pType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, originalChannels);
    }
    if (SUCCEEDED(hr)) {
        hr = pType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    
    if (SUCCEEDED(hr)) {
        hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pType);
    }
    
    if (FAILED(hr)) {
        printf("Failed to configure source reader\n");
        pOriginalType->Release();
        pType->Release();
        pReader->Release();
        return false;
    }
    
    // Verify the configured format
    IMFMediaType* pCurrentType = nullptr;
    hr = pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pCurrentType);
    if (FAILED(hr)) {
        printf("Failed to get current media type\n");
        pOriginalType->Release();
        pType->Release();
        pReader->Release();
        return false;
    }
    
    UINT32 sampleRate = originalSampleRate;  // Use original sample rate
    UINT32 channels = originalChannels;      // Use original channel count
    UINT32 bitsPerSample = 16;
    
    // Double-check the configured values
    pCurrentType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
    pCurrentType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    pCurrentType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample);
    
    printf("Output format: %d Hz, %d channels, %d bits per sample\n", sampleRate, channels, bitsPerSample);
    
    // Warn if there's a mismatch
    if (sampleRate != originalSampleRate) {
        printf("WARNING: Sample rate mismatch! Original: %d, Output: %d\n", originalSampleRate, sampleRate);
    }
    
    // Create WAV file
    FILE* wav_file = fopen(wav_filename, "wb");
    if (!wav_file) {
        printf("Cannot create WAV file: %s\n", wav_filename);
        pCurrentType->Release();
        pOriginalType->Release();
        pType->Release();
        pReader->Release();
        return false;
    }
    
    // Write initial WAV header (we'll update sizes later)
    long riff_size_pos, data_size_pos;
    
    // RIFF header
    fwrite("RIFF", 1, 4, wav_file);
    riff_size_pos = ftell(wav_file);
    uint32_t placeholder = 0;
    fwrite(&placeholder, 4, 1, wav_file); // File size (will update later)
    fwrite("WAVE", 1, 4, wav_file);
    
    // fmt chunk
    fwrite("fmt ", 1, 4, wav_file);
    int fmt_chunk_size = 16;
    short audio_format = 1; // PCM
    int byte_rate = sampleRate * channels * bitsPerSample / 8;
    short block_align = channels * bitsPerSample / 8;
    
    fwrite(&fmt_chunk_size, 4, 1, wav_file);
    fwrite(&audio_format, 2, 1, wav_file);
    fwrite(&channels, 2, 1, wav_file);
    fwrite(&sampleRate, 4, 1, wav_file);
    fwrite(&byte_rate, 4, 1, wav_file);
    fwrite(&block_align, 2, 1, wav_file);
    fwrite(&bitsPerSample, 2, 1, wav_file);
    
    // data chunk header
    fwrite("data", 1, 4, wav_file);
    data_size_pos = ftell(wav_file);
    fwrite(&placeholder, 4, 1, wav_file); // Data size (will update later)
    
    long audio_data_start = ftell(wav_file);
    
    // Read and write audio data
    uint32_t total_bytes_written = 0;
    
    while (SUCCEEDED(hr)) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* pSample = nullptr;
        
        hr = pReader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, 
                                nullptr, &flags, &timestamp, &pSample);
        
        if (FAILED(hr)) break;
        
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            break;
        }
        
        if (pSample) {
            IMFMediaBuffer* pBuffer = nullptr;
            hr = pSample->ConvertToContiguousBuffer(&pBuffer);
            if (SUCCEEDED(hr)) {
                BYTE* pData = nullptr;
                DWORD maxLength = 0, currentLength = 0;
                
                hr = pBuffer->Lock(&pData, &maxLength, &currentLength);
                if (SUCCEEDED(hr)) {
                    fwrite(pData, 1, currentLength, wav_file);
                    total_bytes_written += currentLength;
                    pBuffer->Unlock();
                }
                pBuffer->Release();
            }
            pSample->Release();
        }
    }
    
    // Update WAV header with actual sizes
    uint32_t file_size = total_bytes_written + 36; // Audio data + header size - 8
    
    // Update RIFF chunk size
    fseek(wav_file, riff_size_pos, SEEK_SET);
    fwrite(&file_size, 4, 1, wav_file);
    
    // Update data chunk size
    fseek(wav_file, data_size_pos, SEEK_SET);
    fwrite(&total_bytes_written, 4, 1, wav_file);
    
    fclose(wav_file);
    
    // Cleanup
    pCurrentType->Release();
    pOriginalType->Release();
    pType->Release();
    pReader->Release();
    
    printf("%s conversion complete\n", fileType);
    return true;
}

// Memory-based conversion for any supported audio format
static bool convertAudioToWavInMemory(const std::vector<uint8_t>& audio_data, std::vector<uint8_t>& wav_data, const char* file_extension) {
    // Media Foundation requires actual file access for audio files
    // Create a temporary file for the conversion process
    char temp_path[MAX_PATH];
    char temp_audio_file[MAX_PATH];
    
    if (GetTempPathA(MAX_PATH, temp_path) == 0 ||
        GetTempFileNameA(temp_path, file_extension, 0, temp_audio_file) == 0) {
        printf("Failed to create temporary file path\n");
        return false;
    }
    
    // Write audio data to temporary file
    FILE* temp_file = fopen(temp_audio_file, "wb");
    if (!temp_file) {
        printf("Failed to create temporary audio file\n");
        return false;
    }
    
    if (fwrite(audio_data.data(), 1, audio_data.size(), temp_file) != audio_data.size()) {
        printf("Failed to write audio data to temporary file\n");
        fclose(temp_file);
        DeleteFileA(temp_audio_file);
        return false;
    }
    fclose(temp_file);
    
    // Create temporary WAV file
    char temp_wav_file[MAX_PATH];
    if (GetTempFileNameA(temp_path, "wav", 0, temp_wav_file) == 0) {
        printf("Failed to create temporary WAV file path\n");
        DeleteFileA(temp_audio_file);
        return false;
    }
    
    // Convert using file-based method
    bool success = convertAudioToWav(temp_audio_file, temp_wav_file);
    
    if (success) {
        // Read converted WAV data back into memory
        FILE* wav_file = fopen(temp_wav_file, "rb");
        if (wav_file) {
            fseek(wav_file, 0, SEEK_END);
            long wav_size = ftell(wav_file);
            fseek(wav_file, 0, SEEK_SET);
            
            wav_data.resize(wav_size);
            if (fread(wav_data.data(), 1, wav_size, wav_file) == (size_t)wav_size) {
                printf("Audio to WAV memory conversion complete (%zu bytes)\n", wav_data.size());
            } else {
                success = false;
                printf("Failed to read converted WAV data\n");
            }
            fclose(wav_file);
        } else {
            success = false;
            printf("Failed to read converted WAV file\n");
        }
    }
    
    // Clean up temporary files
    DeleteFileA(temp_audio_file);
    DeleteFileA(temp_wav_file);
    
    return success;
}

// Wrapper functions for backward compatibility
bool convertM4aToWavInMemory(const std::vector<uint8_t>& m4a_data, std::vector<uint8_t>& wav_data) {
    return convertAudioToWavInMemory(m4a_data, wav_data, "m4a");
}

bool convertWmaToWavInMemory(const std::vector<uint8_t>& wma_data, std::vector<uint8_t>& wav_data) {
    return convertAudioToWavInMemory(wma_data, wav_data, "wma");
}

bool convertM4aToWav(const char* m4a_filename, const char* wav_filename) {
    return convertAudioToWav(m4a_filename, wav_filename);
}

bool convertWmaToWav(const char* wma_filename, const char* wav_filename) {
    return convertAudioToWav(wma_filename, wav_filename);
}

bool convert_audio_to_wav(AudioPlayer *player, const char* filename) {
    return convert_audio_to_wav_internal(player, filename);
}

// Internal generic audio converter for Windows Media Foundation
bool convert_audio_to_wav_internal(AudioPlayer *player, const char* filename) {
    // Check cache first
    const char* cached_file = get_cached_conversion(&player->conversion_cache, filename);
    if (cached_file) {
        strncpy(player->temp_wav_file, cached_file, sizeof(player->temp_wav_file) - 1);
        player->temp_wav_file[sizeof(player->temp_wav_file) - 1] = '\0';
        return true;
    }
    
    // Determine file type
    bool isWma = isWmaFile(filename);
    bool isM4a = isM4aFile(filename);
    
    if (!isWma && !isM4a) {
        printf("Unsupported audio format: %s\n", filename);
        return false;
    }
    
    // Generate a unique virtual filename
    static int virtual_counter = 0;
    char virtual_filename[256];
    const char* prefix = isWma ? "virtual_wma" : "virtual_m4a";
    snprintf(virtual_filename, sizeof(virtual_filename), "%s_%d.wav", prefix, virtual_counter++);
    
    strncpy(player->temp_wav_file, virtual_filename, sizeof(player->temp_wav_file) - 1);
    player->temp_wav_file[sizeof(player->temp_wav_file) - 1] = '\0';
    
    printf("Converting %s to virtual WAV: %s -> %s\n", 
           isWma ? "WMA" : "M4A", filename, virtual_filename);
    
    // Read audio file into memory
    FILE* audio_file = fopen(filename, "rb");
    if (!audio_file) {
        printf("Cannot open audio file: %s\n", filename);
        return false;
    }
    
    fseek(audio_file, 0, SEEK_END);
    long audio_size = ftell(audio_file);
    fseek(audio_file, 0, SEEK_SET);
    
    std::vector<uint8_t> audio_data(audio_size);
    if (fread(audio_data.data(), 1, audio_size, audio_file) != (size_t)audio_size) {
        printf("Failed to read audio file\n");
        fclose(audio_file);
        return false;
    }
    fclose(audio_file);
    
    // Convert to WAV in memory
    std::vector<uint8_t> wav_data;
    bool conversion_success = false;
    
    if (isWma) {
        conversion_success = convertWmaToWavInMemory(audio_data, wav_data);
    } else {
        conversion_success = convertM4aToWavInMemory(audio_data, wav_data);
    }
    
    if (!conversion_success) {
        printf("Audio to WAV conversion failed\n");
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
    
    printf("Audio conversion to virtual file complete\n");
    return true;
}

// Windows-specific M4A converter (calls generic function)
bool convert_m4a_to_wav(AudioPlayer *player, const char* filename) {
    return convert_audio_to_wav_internal(player, filename);
}

// Windows-specific WMA converter (calls generic function)
bool convert_wma_to_wav(AudioPlayer *player, const char* filename) {
    return convert_audio_to_wav_internal(player, filename);
}

#endif // _WIN32
