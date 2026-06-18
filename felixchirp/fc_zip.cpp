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
