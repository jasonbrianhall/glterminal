// C++ port of kfn_dumper.c, restructured as a reusable KFNArchive class
// (RAII, per-entry random access, extract-to-temp-file) so it can be
// wired into felixchirp's file browser / audio pipeline. The directory
// parsing and the chunked AES_decrypt loop are unchanged from the
// original — only the I/O plumbing is different (streams to a temp file
// instead of a fixed output path, and reports errors via lastError()
// instead of exit(1)).
#include "kfn.h"
#include <cstdlib>
#include <cstring>
#include <strings.h>   // strcasecmp
#ifndef _WIN32
#  include <unistd.h>
#else
#  include <windows.h>
#endif

bool KFNArchive::readByte(uint8_t &b) {
    return fread(&b, 1, 1, file_) == 1;
}

bool KFNArchive::readDword(uint32_t &v) {
    uint8_t b1, b2, b3, b4;
    if (!readByte(b1) || !readByte(b2) || !readByte(b3) || !readByte(b4)) return false;
    v = ((uint32_t)b4 << 24) | ((uint32_t)b3 << 16) | ((uint32_t)b2 << 8) | b1;
    return true;
}

bool KFNArchive::readBytes(std::vector<uint8_t> &out, size_t len) {
    out.resize(len);
    if (len == 0) return true;
    return fread(out.data(), 1, len, file_) == len;
}

bool KFNArchive::readString(std::string &out, size_t len) {
    std::vector<uint8_t> buf;
    if (!readBytes(buf, len)) return false;
    out.assign((const char *)buf.data(), buf.size());
    return true;
}

bool KFNArchive::open(const std::string &path) {
    close();
    file_ = fopen(path.c_str(), "rb");
    if (!file_) { lastError_ = "Cannot open file"; return false; }

    std::string sig;
    if (!readString(sig, 4) || sig != "KFNB") {
        lastError_ = "Not a KFN file (bad signature)";
        close();
        return false;
    }

    // Header records, terminated by "ENDH". "FLID" carries the AES key.
    for (;;) {
        std::string headerSig;
        uint8_t type = 0;
        uint32_t lenOrValue = 0;
        if (!readString(headerSig, 4) || !readByte(type) || !readDword(lenOrValue)) {
            lastError_ = "Truncated KFN header";
            close();
            return false;
        }

        std::vector<uint8_t> buf;
        bool hasBuf = false;
        if (type == 2) {
            if (!readBytes(buf, lenOrValue)) {
                lastError_ = "Truncated KFN header payload";
                close();
                return false;
            }
            hasBuf = true;
        }

        if (headerSig == "FLID" && hasBuf) {
            // Same call as kfn_dumper.c: derive the decrypt-schedule from
            // the raw 128-bit key straight from the FLID record.
            if (buf.size() >= 16 && AES_set_decrypt_key(buf.data(), 128, &aesKey_) == 0) {
                haveKey_ = true;
            }
        }

        if (headerSig == "ENDH") break;
    }

    uint32_t numFiles = 0;
    if (!readDword(numFiles)) {
        lastError_ = "Truncated KFN directory count";
        close();
        return false;
    }

    entries_.clear();
    entries_.reserve(numFiles);
    for (uint32_t i = 0; i < numFiles; i++) {
        KFNEntry e;
        uint32_t filenameLen = 0;
        if (!readDword(filenameLen) || !readString(e.filename, filenameLen) ||
            !readDword(e.type) || !readDword(e.lengthOut) ||
            !readDword(e.offset) || !readDword(e.lengthIn) || !readDword(e.flags)) {
            lastError_ = "Truncated KFN directory entry";
            close();
            return false;
        }
        entries_.push_back(std::move(e));
    }

    // Entry offsets are stored relative to the end of the directory table
    // (matches kfn_dumper.c: entries[i].offset += dir_end).
    long dirEnd = ftell(file_);
    if (dirEnd < 0) {
        lastError_ = "ftell failed";
        close();
        return false;
    }
    for (auto &e : entries_) {
        e.offset += (uint32_t)dirEnd;
    }

    lastError_.clear();
    return true;
}

void KFNArchive::close() {
    if (file_) { fclose(file_); file_ = nullptr; }
    entries_.clear();
    haveKey_ = false;
}

const KFNEntry *KFNArchive::find(const std::string &filename) const {
    for (auto &e : entries_) {
        if (strcasecmp(e.filename.c_str(), filename.c_str()) == 0) return &e;
    }
    return nullptr;
}

unsigned char *KFNArchive::extract(const KFNEntry &entry, size_t &out_size) {
    out_size = 0;
    if (!file_) { lastError_ = "Archive not open"; return nullptr; }
    if (entry.encrypted() && !haveKey_) { lastError_ = "Encryption key unknown"; return nullptr; }
    if (fseek(file_, (long)entry.offset, SEEK_SET) != 0) { lastError_ = "Seek failed"; return nullptr; }

    unsigned char *raw = (unsigned char *)malloc(entry.lengthIn ? entry.lengthIn : 1);
    if (!raw) { lastError_ = "Out of memory"; return nullptr; }
    if (entry.lengthIn > 0 && fread(raw, 1, entry.lengthIn, file_) != entry.lengthIn) {
        lastError_ = "Short read";
        free(raw);
        return nullptr;
    }

    if (!entry.encrypted()) {
        out_size = entry.lengthOut ? entry.lengthOut : entry.lengthIn;
        return raw;
    }

    // AES-128-ECB, no padding — decrypt per 16-byte block, same as
    // kfn_dumper.c's kfn_extract loop, then trim to the decoded length.
    unsigned char *dec = (unsigned char *)malloc(entry.lengthIn ? entry.lengthIn : 1);
    if (!dec) { lastError_ = "Out of memory"; free(raw); return nullptr; }
    size_t alignedLen = entry.lengthIn - (entry.lengthIn % 16);
    for (size_t i = 0; i < alignedLen; i += 16) {
        AES_decrypt(&raw[i], &dec[i], &aesKey_);
    }
    free(raw);

    out_size = entry.lengthOut < entry.lengthIn ? entry.lengthOut : alignedLen;
    return dec;
}

std::string KFNArchive::extractToTemp(const KFNEntry &entry, const char *ext) {
    if (!file_) { lastError_ = "Archive not open"; return ""; }
    if (entry.encrypted() && !haveKey_) { lastError_ = "Encryption key unknown"; return ""; }
    if (fseek(file_, (long)entry.offset, SEEK_SET) != 0) { lastError_ = "Seek failed"; return ""; }

#ifndef _WIN32
    char path[512];
    snprintf(path, sizeof(path), "/tmp/kfn_tmp_XXXXXX%s", ext ? ext : "");
    int fd = mkstemps(path, ext ? (int)strlen(ext) : 0);
    if (fd < 0) { lastError_ = "mkstemps failed"; return ""; }
    FILE *out = fdopen(fd, "wb");
    if (!out) { ::close(fd); unlink(path); lastError_ = "fdopen failed"; return ""; }
#else
    char tmp_dir[MAX_PATH];
    if (!GetTempPathA(sizeof(tmp_dir), tmp_dir)) { lastError_ = "GetTempPathA failed"; return ""; }
    char base[MAX_PATH];
    if (!GetTempFileNameA(tmp_dir, "kfn_", 0, base)) { lastError_ = "GetTempFileNameA failed"; return ""; }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", base, ext ? ext : "");
    DeleteFileA(base);
    FILE *out = fopen(path, "wb");
    if (!out) { lastError_ = "fopen failed"; return ""; }
#endif

    // Chunked read/decrypt/write loop, same shape as kfn_dumper.c's
    // kfn_extract, so encrypted multi-MB audio/video entries stream
    // straight to disk without needing a second full-size buffer.
    unsigned char buffer[8192];   // must stay a multiple of 16 for AES
    unsigned char decrypted[8192];
    uint32_t total = 0;
    bool ok = true;

    while (total < entry.lengthIn) {
        size_t to_read = sizeof(buffer);
        if (to_read > entry.lengthIn - total) to_read = entry.lengthIn - total;

        size_t bytes_read = fread(buffer, 1, to_read, file_);
        if (bytes_read == 0) break;

        if (entry.encrypted()) {
            size_t alignedLen = bytes_read - (bytes_read % 16);
            for (size_t i = 0; i < alignedLen; i += 16) {
                AES_decrypt(&buffer[i], &decrypted[i], &aesKey_);
            }
            size_t to_write = alignedLen;
            if (total + to_write > entry.lengthOut) {
                to_write = (entry.lengthOut > total) ? (entry.lengthOut - total) : 0;
            }
            if (to_write > 0 && fwrite(decrypted, 1, to_write, out) != to_write) { ok = false; break; }
        } else {
            if (fwrite(buffer, 1, bytes_read, out) != bytes_read) { ok = false; break; }
        }

        total += (uint32_t)bytes_read;
    }

    fclose(out);
    if (!ok) {
#ifndef _WIN32
        unlink(path);
#else
        DeleteFileA(path);
#endif
        lastError_ = "Write failed while extracting entry";
        return "";
    }
    return std::string(path);
}
