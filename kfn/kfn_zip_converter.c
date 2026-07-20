/*
 * KFN v1 Zip Converter
 * 
 * A reverse-engineering and format conversion tool for KFN v1 (Karaoke File Normal)
 * archive files used by legacy karaoke applications (Karafun player < v4.x).
 * 
 * KFN v1 is an obsolete format superseded by KFN v2 in current player releases.
 * This tool documents and converts KFN v1 archives into standard ZIP format for
 * archival, research, and format migration purposes.
 * 
 * KFN v1 files are container formats that bundle multimedia resources (audio, video,
 * images, fonts, metadata) into a single file with optional AES-128 encryption.
 * 
 * This tool parses the KFN v1 file structure and extracts all contained resources,
 * packaging them into a standard ZIP archive for easier access and analysis.
 * 
 * Features:
 * - Parses KFN v1 file headers and directory structure
 * - Supports both uncompressed and AES-128 encrypted resources
 * - Extracts all resource types (audio, video, images, fonts, text, effects)
 * - Creates standard ZIP archive output
 * - Handles metadata and resource information
 * 
 * Usage:
 *   kfn_zip_converter <input.kfn> [output.zip]
 * 
 * Compilation:
 *   gcc kfn_zip_converter.c -o kfn_zip_converter -lssl -lcrypto -lzip
 * 
 * Dependencies:
 *   - libssl/libcrypto (OpenSSL) for AES decryption
 *   - libzip for ZIP archive creation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <openssl/aes.h>
#include <zip.h>

/* Resource type constants */
#define TYPE_SONGTEXT 1
#define TYPE_MUSIC 2
#define TYPE_IMAGE 3
#define TYPE_FONT 4
#define TYPE_VIDEO 5
#define TYPE_MILK 6  /* Milk video effect format */

/* Represents a resource entry in the KFN archive */
typedef struct {
	int type;           /* Resource type (see TYPE_* constants) */
	char *filename;     /* Resource filename/path */
	int length_in;      /* Compressed or encrypted size */
	int length_out;     /* Decompressed or decrypted size */
	int offset;         /* Byte offset in file where data begins */
	int flags;          /* Flags: bit 0 = encrypted */
} Entry;

/* Main context for KFN file processing */
typedef struct {
	FILE *file;                 /* Open KFN file handle */
	AES_KEY aes_key;            /* AES decryption key (if present) */
	int has_key;                /* Flag: whether AES key was found */
	struct zip *za;             /* Output ZIP archive handle */
} KFNDumper;

/*
 * Helper I/O Functions
 * 
 * These functions handle low-level file reading with proper error handling.
 */

/* Read a single byte from the KFN file */
static uint8_t read_byte(KFNDumper *dumper) {
	uint8_t b;
	if (fread(&b, 1, 1, dumper->file) != 1) {
		fprintf(stderr, "Error reading byte\n");
		exit(1);
	}
	return b;
}

/* Read a 32-bit little-endian integer from the KFN file */
static uint32_t read_dword(KFNDumper *dumper) {
	uint8_t b1 = read_byte(dumper);
	uint8_t b2 = read_byte(dumper);
	uint8_t b3 = read_byte(dumper);
	uint8_t b4 = read_byte(dumper);

	return (b4 << 24) | (b3 << 16) | (b2 << 8) | b1;
}

/* Read arbitrary number of bytes from the KFN file. Caller must free returned buffer. */
static uint8_t *read_bytes(KFNDumper *dumper, int length) {
	uint8_t *buffer = (uint8_t *)malloc(length);
	if (!buffer) {
		fprintf(stderr, "Memory allocation failed\n");
		exit(1);
	}

	if (fread(buffer, 1, length, dumper->file) != length) {
		fprintf(stderr, "Error reading %d bytes\n", length);
		free(buffer);
		exit(1);
	}

	return buffer;
}

/*
 * Initialize a new KFN dumper context for the given file.
 * Returns NULL if the file cannot be opened.
 */
KFNDumper *kfn_dumper_new(const char *filename) {
	KFNDumper *dumper = (KFNDumper *)malloc(sizeof(KFNDumper));
	if (!dumper) {
		fprintf(stderr, "Memory allocation failed\n");
		return NULL;
	}

	dumper->file = fopen(filename, "rb");
	if (!dumper->file) {
		fprintf(stderr, "Cannot open file: %s\n", filename);
		free(dumper);
		return NULL;
	}

	dumper->has_key = 0;
	dumper->za = NULL;
	return dumper;
}

/* Clean up KFN dumper resources (closes file and ZIP archive) */
void kfn_dumper_free(KFNDumper *dumper) {
	if (dumper) {
		if (dumper->za)
			zip_close(dumper->za);
		if (dumper->file)
			fclose(dumper->file);
		free(dumper);
	}
}

/*
 * Parse the KFN file structure and extract all resource entries.
 * Reads the file header, extracts AES key if present, and builds
 * an array of resource entries.
 * 
 * KFN File Structure:
 * ==================
 * 1. Magic bytes: "KFNB" (4 bytes)
 * 
 * 2. Header section (variable length):
 *    - Repeating header fields until "ENDH" marker
 *    - Each field: signature (4 bytes) + type (1 byte) + size/value (4 bytes) + data
 *    - Type 1: 4-byte integer value
 *    - Type 2: Binary data of specified size
 *    - Notable fields:
 *      * FLID: Binary data containing AES-128 encryption key (16 bytes)
 *      * ENDH: Marks end of header section
 * 
 * 3. Directory section:
 *    - File count (4 bytes, little-endian uint32)
 *    - For each file:
 *      * Filename length (4 bytes)
 *      * Filename (variable length, UTF-8 or ASCII)
 *      * Type (4 bytes) - see TYPE_* constants
 *      * Decompressed size (4 bytes)
 *      * Offset (4 bytes) - relative to end of directory
 *      * Compressed/encrypted size (4 bytes)
 *      * Flags (4 bytes) - bit 0 = encrypted with AES-128
 * 
 * 4. Data section:
 *    - Raw file data at offsets specified in directory
 *    - Encrypted files use AES-128 ECB mode
 * 
 * Returns array of Entry structs. Caller must free each filename and the array.
 * Sets out_count to the number of entries found.
 */
Entry *kfn_list(KFNDumper *dumper, int *out_count) {
	*out_count = 0;

	/* Read and verify magic bytes "KFNB" */
	uint8_t *sig = read_bytes(dumper, 4);
	if (strncmp((char *)sig, "KFNB", 4) != 0) {
		fprintf(stderr, "Error: Not a valid KFN file (magic bytes not found)\n");
		free(sig);
		return NULL;
	}
	free(sig);

	/* Parse header fields until ENDH marker
	 * 
	 * Header field format:
	 *   Offset  Size  Field
	 *   0       4     Field signature (e.g., "FLID", "INFO")
	 *   4       1     Field type (1 = int, 2 = binary)
	 *   5       4     Length (if type 2) or value (if type 1)
	 *   9       ?     Data (only if type 2)
	 */
	while (1) {
		sig = read_bytes(dumper, 4);
		uint8_t type = read_byte(dumper);
		uint32_t len_or_value = read_dword(dumper);
		uint8_t *buf = NULL;

		/* Type 2 = binary data, read len_or_value bytes */
		if (type == 2) {
			buf = read_bytes(dumper, len_or_value);
		}

		/* FLID field contains the AES-128 encryption key
		 * - 16 bytes for AES-128 (128-bit key)
		 * - Used for decrypting resources with encrypted flag set
		 * - Key is stored unencrypted in the file header
		 */
		if (strncmp((char *)sig, "FLID", 4) == 0 && buf != NULL) {
			if (len_or_value == 16) {
				AES_set_decrypt_key(buf, 128, &dumper->aes_key);
				dumper->has_key = 1;
			}
		}

		if (buf)
			free(buf);

		/* ENDH marks the end of header section, followed by directory */
		if (strncmp((char *)sig, "ENDH", 4) == 0) {
			free(sig);
			break;
		}

		free(sig);
	}

	/* Read number of files in directory
	 * This is stored as a little-endian 32-bit unsigned integer
	 */
	uint32_t num_files = read_dword(dumper);

	/* Allocate array to hold all resource entries */
	Entry *entries = (Entry *)malloc(num_files * sizeof(Entry));
	if (!entries) {
		fprintf(stderr, "Memory allocation failed\n");
		return NULL;
	}

	/* Parse directory entries
	 * 
	 * Each entry contains metadata for one resource in the file.
	 * Offsets are relative to the end of the directory section,
	 * allowing the format to be extensible.
	 */
	for (uint32_t i = 0; i < num_files; i++) {
		/* Read filename */
		uint32_t filename_len = read_dword(dumper);
		uint8_t *filename = read_bytes(dumper, filename_len);

		entries[i].filename = (char *)malloc(filename_len + 1);
		memcpy(entries[i].filename, filename, filename_len);
		entries[i].filename[filename_len] = '\0';
		free(filename);

		/* Read resource metadata
		 * 
		 * Fields in order:
		 *   type       - Resource type (TYPE_MUSIC, TYPE_IMAGE, etc.)
		 *   length_out - Size after decompression/decryption
		 *   offset     - Byte offset from directory end to data
		 *   length_in  - Size as stored (encrypted/compressed)
		 *   flags      - Bit 0: encrypted flag
		 */
		entries[i].type = read_dword(dumper);
		entries[i].length_out = read_dword(dumper);
		entries[i].offset = read_dword(dumper);
		entries[i].length_in = read_dword(dumper);
		entries[i].flags = read_dword(dumper);
	}

	/* Convert relative offsets to absolute file positions
	 * 
	 * Stored offsets are relative to the end of the directory section.
	 * Calculate absolute position: absolute = relative + directory_end
	 * This is necessary because the header size varies.
	 */
	long dir_end = ftell(dumper->file);
	for (uint32_t i = 0; i < num_files; i++) {
		entries[i].offset += dir_end;
	}

	*out_count = num_files;
	return entries;
}

/*
 * Extract a single resource from the KFN file and add it to the output ZIP archive.
 * Handles both encrypted and unencrypted resources.
 * 
 * Encryption Details (AES-128 ECB Mode):
 * =======================================
 * - Algorithm: AES-128 in ECB (Electronic Codebook) mode
 * - Key: 128-bit key extracted from FLID header field
 * - Block size: 16 bytes (128 bits)
 * - Processing: Each 16-byte block is decrypted independently
 * - Padding: Handled by length_out field (actual decompressed size)
 * 
 * Why ECB mode was likely chosen:
 *   - Simpler to implement (no IV needed, no state management)
 *   - Random access within file (each block independent)
 *   - File structure is not security-critical
 * 
 * Security implications:
 *   - Key is stored unencrypted in file header (FLID field)
 *   - ECB mode reveals patterns in encrypted data
 *   - Not suitable for security-critical applications
 *   - Serves more as obfuscation/integrity check
 * 
 * If encryption is required but no key is available, prints error and returns.
 */
void kfn_extract(KFNDumper *dumper, Entry *entry) {
	/* Check if resource is encrypted and key is available
	 * Bit 0 of flags field indicates encryption status
	 */
	if ((entry->flags & 0x01) && !dumper->has_key) {
		fprintf(stderr, "Error: Resource '%s' is encrypted but no key found in file\n", 
		        entry->filename);
		return;
	}

	/* Seek to the resource data within the file */
	fseek(dumper->file, entry->offset, SEEK_SET);

	/* Allocate buffer for decompressed/decrypted output
	 * length_out is the final size after processing
	 */
	uint8_t *out_buffer = (uint8_t *)malloc(entry->length_out);
	if (!out_buffer) {
		fprintf(stderr, "Memory allocation failed\n");
		return;
	}

	/* Read and process data in 8KB chunks
	 * Buffer size must be multiple of 16 for AES block processing
	 */
	uint8_t buffer[8192];
	int total = 0;

	while (total < entry->length_in) {
		/* Calculate how much to read in this iteration */
		int to_read = sizeof(buffer);
		if (to_read > entry->length_in - total)
			to_read = entry->length_in - total;

		/* Read chunk from KFN file */
		size_t bytes_read = fread(buffer, 1, to_read, dumper->file);
		if (bytes_read == 0)
			break;

		if (entry->flags & 0x01) {
			/* Resource is encrypted - decrypt it
			 * 
			 * AES-128 ECB decryption:
			 *   - Process data in 16-byte blocks
			 *   - Each block decrypted independently
			 *   - Pad last block if needed (handled by padding calculation)
			 */
			uint8_t decrypted[8192];
			for (size_t i = 0; i < bytes_read; i += 16) {
				AES_decrypt(&buffer[i], &decrypted[i], &dumper->aes_key);
			}

			/* Write decrypted data to output buffer
			 * Respect length_out boundary (padding may add extra bytes)
			 */
			int to_write = bytes_read;
			if (total + to_write > entry->length_out)
				to_write = entry->length_out - total;

			memcpy(&out_buffer[total], decrypted, to_write);
			total += to_write;
		} else {
			/* Resource is unencrypted - copy directly */
			int to_write = bytes_read;
			if (total + to_write > entry->length_out)
				to_write = entry->length_out - total;

			memcpy(&out_buffer[total], buffer, to_write);
			total += to_write;
		}
	}

	/* Add extracted resource to output ZIP archive
	 * 
	 * Parameters:
	 *   dumper->za      - ZIP archive handle
	 *   out_buffer      - Data to add
	 *   total           - Actual bytes written
	 *   1               - Free buffer when done (libzip takes ownership)
	 */
	struct zip_source *source = zip_source_buffer(dumper->za, out_buffer, total, 1);
	if (source == NULL) {
		fprintf(stderr, "Failed to create zip source for %s\n", entry->filename);
		free(out_buffer);
		return;
	}

	/* Add the resource to the ZIP archive */
	if (zip_file_add(dumper->za, entry->filename, source, ZIP_FL_ENC_UTF_8) < 0) {
		fprintf(stderr, "Failed to add file %s to zip: %s\n", entry->filename, 
		        zip_strerror(dumper->za));
		zip_source_free(source);
		free(out_buffer);
		return;
	}
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printf("KFN Zip Converter - Convert KFN archive files to standard ZIP format\n\n");
		printf("Usage: %s <input.kfn> [output.zip]\n", argv[0]);
		printf("  input.kfn   - Path to the KFN file to convert\n");
		printf("  output.zip  - Path to save the ZIP archive (default: output.zip)\n\n");
		printf("Example:\n");
		printf("  %s song.kfn converted.zip\n", argv[0]);
		return 1;
	}

	/* Set default output filename if not provided */
	const char *output_zip = (argc > 2) ? argv[2] : "output.zip";

	printf("Opening KFN file: %s\n", argv[1]);
	printf("Output will be saved to: %s\n\n", output_zip);

	/* Create zip archive */
	int err = 0;
	struct zip *za = zip_open(output_zip, ZIP_CREATE | ZIP_TRUNCATE, &err);
	if (!za) {
		fprintf(stderr, "Error: Cannot create zip file: %s\n", output_zip);
		return 1;
	}

	/* Initialize KFN parser */
	KFNDumper *dumper = kfn_dumper_new(argv[1]);
	if (!dumper) {
		zip_close(za);
		return 1;
	}

	dumper->za = za;

	/* Parse KFN file and extract all resources */
	int num_entries = 0;
	Entry *entries = kfn_list(dumper, &num_entries);

	if (entries) {
		printf("Found %d resources:\n\n", num_entries);
		for (int i = 0; i < num_entries; i++) {
			Entry *entry = &entries[i];
			const char *type_str = "";
			switch (entry->type) {
				case TYPE_SONGTEXT: type_str = "Song Text"; break;
				case TYPE_MUSIC: type_str = "Music"; break;
				case TYPE_IMAGE: type_str = "Image"; break;
				case TYPE_FONT: type_str = "Font"; break;
				case TYPE_VIDEO: type_str = "Video"; break;
				case TYPE_MILK: type_str = "Milk Effect"; break;
				default: type_str = "Unknown"; break;
			}
			printf("  [%d/%d] %s (%s, %d bytes)\n",
				   i+1, num_entries, entry->filename, type_str, entry->length_out);

			kfn_extract(dumper, entry);
		}

		printf("\n");

		/* Cleanup */
		for (int i = 0; i < num_entries; i++) {
			free(entries[i].filename);
		}
		free(entries);
	} else {
		fprintf(stderr, "Error: No valid KFN file structure found\n");
		kfn_dumper_free(dumper);
		zip_close(za);
		return 1;
	}

	kfn_dumper_free(dumper);
	printf("Successfully saved archive to: %s\n", output_zip);
	return 0;
}
