#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ctype.h>

#include <cstdint>
#include "felixchirp.h"
#include "../gl_renderer.h"
#include "../sdl_renderer.h"
#include "../ft_font.h"
#include "../term_color.h"

#include "miniz_common.h"
#include "miniz_export.h"
#include "miniz.h"
#include "miniz_tdef.h"
#include "miniz_tinfl.h"
#include "miniz_zip.h"


// Returns true if the zip at zip_path contains a CDG+audio pair
bool zip_contains_cdg_pair(const char *zip_path) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) return false;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    std::vector<std::string> names;
    for (mz_uint i = 0; i < n; i++) {
        char fname[512] = {};
        mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
        names.push_back(fname);
    }
    mz_zip_reader_end(&zip);
    for (auto &nm : names) {
        if (!is_audio_ext(nm.c_str())) continue;
        char base[512]; strncpy(base, nm.c_str(), sizeof(base)-1);
        char *dot = strrchr(base, '.'); if (!dot) continue; *dot = '\0';
        for (auto &nm2 : names) {
            const char *d2 = strrchr(nm2.c_str(), '.');
            if (!d2) continue;
            char e2[8] = {};
            for (int i = 0; i < 7 && d2[i]; i++) e2[i] = tolower((unsigned char)d2[i]);
            if (strcmp(e2, ".cdg") != 0) continue;
            // Check base names match
            size_t blen = strlen(base);
            if (nm2.size() >= blen + 4 && strncasecmp(nm2.c_str(), base, blen) == 0)
                return true;
        }
    }
    return false;
}

// List images inside a local zip file into out.
// Each entry has is_zip_entry=true, zip_path set to zip_filepath, zip_entry set to the member name.
void iv_list_zip(const char *zip_filepath, std::vector<IVEntry> &out) {
    out.clear();
    IVEntry up{}; strncpy(up.name, "..", sizeof(up.name)-1); up.is_dir = true;
    out.push_back(up);

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_file(&zip, zip_filepath, 0)) return;

    mz_uint n = mz_zip_reader_get_num_files(&zip);

    // First pass: collect all filenames so we can detect CDG pairs
    std::vector<std::string> all_names;
    for (mz_uint i = 0; i < n; i++) {
        char fname[512] = {};
        mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
        all_names.push_back(fname);
    }

    // Helper: check if a matching .cdg exists inside this zip for a given audio entry
    auto zip_has_cdg_pair = [&](const char *audio_fname) -> bool {
        char base[512]; strncpy(base, audio_fname, sizeof(base)-1);
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        for (auto &nm : all_names) {
            const char *ndot = strrchr(nm.c_str(), '.');
            if (!ndot) continue;
            // Compare base names case-insensitively
            size_t blen = strlen(base);
            if (nm.size() < blen + 4) continue;
            if (strncasecmp(nm.c_str(), base, blen) == 0) {
                char ext[8] = {};
                for (int j = 0; j < 7 && ndot[j]; j++) ext[j] = tolower((unsigned char)ndot[j]);
                if (strcmp(ext, ".cdg") == 0) return true;
            }
        }
        return false;
    };

    for (mz_uint i = 0; i < n; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        char fname[512] = {};
        mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
        if (strncmp(fname, "__MACOSX", 8) == 0) continue;

        bool img   = is_image_ext(fname);
        bool txt   = is_text_ext(fname);
        bool md    = is_markdown_ext(fname);
        bool audio = is_audio_ext(fname);
        bool cdg   = is_cdg_ext(fname);
        if (!img && !txt && !md && !audio && !cdg) continue;

        mz_zip_archive_file_stat st;
        mz_zip_reader_file_stat(&zip, i, &st);

        IVEntry e{};
        const char *slash = strrchr(fname, '/');
        strncpy(e.name,      slash ? slash+1 : fname, sizeof(e.name)-1);
        strncpy(e.zip_path,  zip_filepath,             sizeof(e.zip_path)-1);
        strncpy(e.zip_entry, fname,                    sizeof(e.zip_entry)-1);
        e.is_zip_entry = true;
        e.size         = st.m_uncomp_size;
        if (audio) { e.is_audio = true; e.has_cdg_pair = zip_has_cdg_pair(fname); }
        if (cdg)   { e.is_cdg   = true; }
        out.push_back(e);
    }
    mz_zip_reader_end(&zip);

    std::stable_sort(out.begin()+1, out.end(), [](const IVEntry &a, const IVEntry &b){
        return strcmp(a.zip_entry, b.zip_entry) < 0;
    });
}

// Extract one image from a local zip into a heap buffer. Caller must free().
unsigned char *iv_extract_zip_entry(const char *zip_filepath, const char *entry_name, size_t &out_size) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    out_size = 0;
    if (!mz_zip_reader_init_file(&zip, zip_filepath, 0)) return nullptr;
    void *buf = mz_zip_reader_extract_file_to_heap(&zip, entry_name, &out_size, 0);
    mz_zip_reader_end(&zip);
    return (unsigned char *)buf;
}
