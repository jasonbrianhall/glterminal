# KFN v1 Zip Converter

A reverse-engineering and format conversion tool for KFN v1 (Karaoke File Normal) archives.

## Project Purpose

This project is a **file format research and documentation effort** aimed at understanding the KFN v1 archive format used by older karaoke applications (primarily Karafun player versions < 4.x). The goal is to:

- Document the KFN v1 file structure through code
- Enable format interoperability by converting KFN v1 archives to standard ZIP format
- Provide a reference implementation for parsing legacy KFN v1 files
- Support educational research into proprietary file formats and their evolution

## Format Status

**KFN v1 is a legacy format** that is no longer supported by current versions of the Karafun player (v4.x and later use KFN v2). This tool focuses on reverse-engineering and documenting this obsolete format for:
- Historical/archival purposes
- Understanding format design evolution
- Converting legacy files to modern formats
- Educational study of file format architecture

## KFN v1 File Format Overview

KFN v1 is a container archive format that bundles multimedia resources for older karaoke applications. This format has been superseded by KFN v2 in current Karafun player releases.

### Structure

```
[Header Section]
  Magic: "KFNB" (4 bytes)
  Header Fields (variable length)
    - Each field has: signature (4 bytes) + type (1 byte) + length/value (4 bytes) + data
    - Field types: 1 = integer, 2 = binary data
    - Terminates with "ENDH" signature
  
[Directory Section]
  File Count (4 bytes, little-endian)
  For each file:
    - Filename length (4 bytes)
    - Filename (variable)
    - Type (4 bytes) - see resource types below
    - Decompressed size (4 bytes)
    - Offset (4 bytes) - relative to directory end
    - Compressed size (4 bytes)
    - Flags (4 bytes) - bit 0: encrypted
  
[Data Section]
  Raw resource data at offsets specified in directory
```

### Resource Types

| Type | Value | Description |
|------|-------|-------------|
| Song Text | 1 | Lyrics/metadata (INI format) |
| Music | 2 | Audio tracks (typically OGG Vorbis) |
| Image | 3 | Graphics/backgrounds (PNG, JPG) |
| Font | 4 | TrueType fonts (TTF) |
| Video | 5 | Video files (MP4, AVI) |
| Milk Effect | 6 | Milk visualization effects |

### Encryption

**Key Storage:** The AES-128 encryption key is stored in the file header under the "FLID" field as binary data.

**Encryption Scheme:**
- Algorithm: AES-128 ECB mode
- Key: Extracted from FLID header field
- Block size: 128 bits (16 bytes)
- Data blocks are encrypted individually

**Security Note:** The encryption key is stored unprotected within the file itself, making the encryption primarily a integrity/obfuscation mechanism rather than a security measure. Files can be decrypted by any party with access to the KFN file.

### Header Fields

Common header fields found in KFN files:

| Field | Type | Purpose |
|-------|------|---------|
| FLID | Binary (2) | Encryption key (16 bytes for AES-128) |
| Other | Varies | Application-specific metadata |

## Building

### Requirements

- GCC or compatible C compiler
- OpenSSL development libraries (`libssl-dev` or `openssl-devel`)
- libzip development libraries (`libzip-dev` or `libzip-devel`)

### Compilation

```bash
gcc kfn_zip_converter.c -o kfn_zip_converter -lssl -lcrypto -lzip
```

### Installation (optional)

```bash
sudo cp kfn_zip_converter /usr/local/bin/
```

## Usage

### Basic Usage

```bash
./kfn_zip_converter <input.kfn> [output.zip]
```

### Examples

Convert a KFN file to ZIP with default output name:
```bash
./kfn_zip_converter song.kfn
# Creates: output.zip
```

Specify custom output filename:
```bash
./kfn_zip_converter song.kfn song_extracted.zip
```

### Output

The tool will:
1. Parse the KFN file structure
2. Extract the encryption key (if present)
3. Decrypt resources as needed
4. List all found resources with their types and sizes
5. Create a standard ZIP archive containing all extracted resources

Example output:
```
Opening KFN file: song.kfn
Output will be saved to: song.zip

Found 10 resources:

  [1/10] OldSansBlack.ttf (Font, 41652 bytes)
  [2/10] flash etoile.png (Image, 105392 bytes)
  [3/10] instru.ogg (Music, 1264922 bytes)
  [4/10] Song.ini (Song Text, 67465 bytes)

Successfully saved archive to: song.zip
```

## File Format Research Notes

### Discovery Process

The KFN format was reverse-engineered by:
1. Analyzing binary file structures
2. Identifying magic bytes and signatures
3. Tracing offset calculations and directory structure
4. Documenting resource types through file content analysis

### Known Limitations

- Compression: Current implementation assumes resources are stored uncompressed or encrypted, not compressed with additional codecs
- Format variants: Different karaoke applications may have format variations
- Future extensions: Unknown header fields are parsed but not interpreted

### Potential Improvements

- Support for additional compression formats
- Better handling of application-specific metadata
- Performance optimization for large archives

## Educational Use

This project is intended for:
- **Format Documentation**: Understanding proprietary file formats through reverse-engineering
- **Interoperability**: Converting between KFN and standard formats
- **Software Development**: Reference implementation for KFN parsing
- **Academic Research**: Study of file format design and encryption patterns

## Legal Disclaimer

This tool is provided for educational and research purposes. Users are responsible for ensuring their use complies with applicable laws and terms of service of any software using KFN files.

The encryption in KFN files is not designed to provide security (the key is stored in the file). This tool's functionality is comparable to standard file format conversion utilities.

## Technical Details

### AES Decryption

Resources marked with the encrypted flag (bit 0 in flags field) are decrypted using:
- Algorithm: AES-128 ECB mode
- Key: 128-bit key from FLID header field
- Process: Each 16-byte block is decrypted individually
- Padding: Handled by length_out field (actual data size after decryption)

### Directory Offset Calculation

Resource offsets in the directory are stored relative to the end of the directory section:
```c
absolute_offset = stored_offset + directory_end_position
```

This allows the file format to be extensible without breaking existing offset references.

## Contributing

If you discover new information about the KFN format, please document it:
- Add comments to the code
- Update this README
- Include examples or test files if possible

## References

- OpenSSL AES Documentation: https://www.openssl.org/docs/manmaster/man3/AES_encrypt.html
- libzip Documentation: https://libzip.org/documentation/
- File Format Research: https://wiki.fileformat.com/ (search for KFN if documented)

## License

This research project and reference implementation are provided as-is for educational purposes.

## Author Notes

This format documentation was created through reverse-engineering and experimental analysis. The KFN format appears to be used primarily by karaoke application vendors for packaging multimedia resources. The relatively simple structure and embedded encryption key suggest this format prioritizes ease of development over security.
