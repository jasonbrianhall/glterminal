#pragma once
// .kfn (kfn) container reader — C++ port of kfn_dumper.c for felixchirp.
//
// Format recap:
//   4 bytes   "KFNB" signature
//   header records: 4-byte tag, 1-byte type, 4-byte little-endian length/value,
//                    then `length` bytes of payload if type==2. Tag "FLID"
//                    (type 2) carries the 16-byte AES-128 key used for
//                    encrypted entries. Terminated by tag "ENDH".
//   4 bytes   number of directory entries
//   per entry: 4-byte filename length, filename bytes, then type, lengthOut
//              (decoded size), offset (relative to end of directory),
//              lengthIn (bytes stored on disk), flags (bit 0 = AES-128-ECB
//              encrypted, no padding).
//
// Decryption reuses OpenSSL's AES_decrypt exactly as in kfn_dumper.c,
// applied per 16-byte block while streaming out to a temp file.
#include <openssl/aes.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

enum KFNEntryType {
    KFN_TYPE_SONGTEXT = 1,
    KFN_TYPE_MUSIC    = 2,
    KFN_TYPE_IMAGE    = 3,
    KFN_TYPE_FONT     = 4,
    KFN_TYPE_VIDEO    = 5,
};

struct KFNEntry {
    std::string filename;
    uint32_t    type      = 0;
    uint32_t    lengthOut = 0;   // decoded size
    uint32_t    offset    = 0;   // absolute offset into the .kfn file
    uint32_t    lengthIn  = 0;   // bytes actually stored on disk
    uint32_t    flags     = 0;

    bool encrypted() const { return (flags & 0x01) != 0; }
};

class KFNArchive {
public:
    ~KFNArchive() { close(); }

    // Parses the header + directory. Returns false on error (see lastError()).
    bool open(const std::string &path);
    void close();

    bool isOpen() const { return file_ != nullptr; }
    const std::vector<KFNEntry> &entries() const { return entries_; }
    bool hasKey() const { return haveKey_; }
    const std::string &lastError() const { return lastError_; }

    // Finds an entry by filename (case-insensitive). Returns nullptr if absent.
    const KFNEntry *find(const std::string &filename) const;

    // Extracts one entry into a heap buffer allocated with malloc(); caller
    // must free() it. Returns nullptr on failure.
    unsigned char *extract(const KFNEntry &entry, size_t &out_size);

    // Streams an entry straight to a new temp file (decrypting on the fly,
    // same chunked approach as kfn_dumper.c's kfn_extract). Caller deletes
    // the file when done (iv_delete_tempfile). Returns "" on failure.
    std::string extractToTemp(const KFNEntry &entry, const char *ext);

private:
    FILE       *file_ = nullptr;
    AES_KEY     aesKey_{};
    bool        haveKey_ = false;
    std::vector<KFNEntry> entries_;
    std::string lastError_;

    bool readByte(uint8_t &b);
    bool readDword(uint32_t &v);
    bool readBytes(std::vector<uint8_t> &out, size_t len);
    bool readString(std::string &out, size_t len);
};
