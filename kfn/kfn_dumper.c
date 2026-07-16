#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <openssl/aes.h>

#define TYPE_SONGTEXT 1
#define TYPE_MUSIC 2
#define TYPE_IMAGE 3
#define TYPE_FONT 4
#define TYPE_VIDEO 5

typedef struct {
	int type;
	char *filename;
	int length_in;
	int length_out;
	int offset;
	int flags;
} Entry;

typedef struct {
	FILE *file;
	AES_KEY aes_key;
	int has_key;
} KFNDumper;

// Helper I/O functions
static uint8_t read_byte(KFNDumper *dumper) {
	uint8_t b;
	if (fread(&b, 1, 1, dumper->file) != 1) {
		fprintf(stderr, "Error reading byte\n");
		exit(1);
	}
	return b;
}

static uint32_t read_dword(KFNDumper *dumper) {
	uint8_t b1 = read_byte(dumper);
	uint8_t b2 = read_byte(dumper);
	uint8_t b3 = read_byte(dumper);
	uint8_t b4 = read_byte(dumper);

	return (b4 << 24) | (b3 << 16) | (b2 << 8) | b1;
}

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
	return dumper;
}

void kfn_dumper_free(KFNDumper *dumper) {
	if (dumper) {
		if (dumper->file)
			fclose(dumper->file);
		free(dumper);
	}
}

Entry *kfn_list(KFNDumper *dumper, int *out_count) {
	*out_count = 0;

	// Read file signature
	uint8_t *sig = read_bytes(dumper, 4);
	if (strncmp((char *)sig, "KFNB", 4) != 0) {
		free(sig);
		return NULL;
	}
	free(sig);

	// Parse header fields
	while (1) {
		sig = read_bytes(dumper, 4);
		uint8_t type = read_byte(dumper);
		uint32_t len_or_value = read_dword(dumper);
		uint8_t *buf = NULL;

		if (type == 2) {
			buf = read_bytes(dumper, len_or_value);
		}

		// Store AES key if we have it
		if (strncmp((char *)sig, "FLID", 4) == 0 && buf != NULL) {
			AES_set_decrypt_key(buf, 128, &dumper->aes_key);
			dumper->has_key = 1;
		}

		if (buf)
			free(buf);

		if (strncmp((char *)sig, "ENDH", 4) == 0) {
			free(sig);
			break;
		}

		free(sig);
	}

	// Read number of files
	uint32_t num_files = read_dword(dumper);

	// Allocate entry array
	Entry *entries = (Entry *)malloc(num_files * sizeof(Entry));
	if (!entries) {
		fprintf(stderr, "Memory allocation failed\n");
		return NULL;
	}

	// Parse directory
	for (uint32_t i = 0; i < num_files; i++) {
		uint32_t filename_len = read_dword(dumper);
		uint8_t *filename = read_bytes(dumper, filename_len);

		entries[i].filename = (char *)malloc(filename_len + 1);
		memcpy(entries[i].filename, filename, filename_len);
		entries[i].filename[filename_len] = '\0';
		free(filename);

		entries[i].type = read_dword(dumper);
		entries[i].length_out = read_dword(dumper);
		entries[i].offset = read_dword(dumper);
		entries[i].length_in = read_dword(dumper);
		entries[i].flags = read_dword(dumper);
	}

	// Adjust offsets based on directory end
	long dir_end = ftell(dumper->file);
	for (uint32_t i = 0; i < num_files; i++) {
		entries[i].offset += dir_end;
	}

	*out_count = num_files;
	return entries;
}

void kfn_extract(KFNDumper *dumper, Entry *entry, const char *outfilename) {
	// Check if we need the decryptor
	if ((entry->flags & 0x01) && !dumper->has_key) {
		fprintf(stderr, "Key is unknown\n");
		return;
	}

	// Seek to file beginning
	fseek(dumper->file, entry->offset, SEEK_SET);

	// Create output file
	FILE *output = fopen(outfilename, "wb");
	if (!output) {
		fprintf(stderr, "Cannot open output file: %s\n", outfilename);
		return;
	}

	uint8_t buffer[8192];  // Must be multiple of 16 for AES
	int total = 0;

	while (total < entry->length_in) {
		int to_read = sizeof(buffer);
		if (to_read > entry->length_in - total)
			to_read = entry->length_in - total;

		size_t bytes_read = fread(buffer, 1, to_read, dumper->file);
		if (bytes_read == 0)
			break;

		if (entry->flags & 0x01) {
			// Decrypt the data
			uint8_t decrypted[8192];
			for (size_t i = 0; i < bytes_read; i += 16) {
				AES_decrypt(&buffer[i], &decrypted[i], &dumper->aes_key);
			}

			// Write decrypted data (might be less due to padding)
			int to_write = bytes_read;
			if (total + to_write > entry->length_out)
				to_write = entry->length_out - total;

			fwrite(decrypted, 1, to_write, output);
		} else {
			fwrite(buffer, 1, bytes_read, output);
		}

		total += bytes_read;
	}

	fclose(output);
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printf("Usage: %s <KFN file>\n", argv[0]);
		return 1;
	}

	KFNDumper *dumper = kfn_dumper_new(argv[1]);
	if (!dumper)
		return 1;

	int num_entries = 0;
	Entry *entries = kfn_list(dumper, &num_entries);

	if (entries) {
		for (int i = 0; i < num_entries; i++) {
			Entry *entry = &entries[i];
			printf("File %s, type: %d, length_in: %d, length_out: %d, offset: %d, flags: %d\n",
				   entry->filename, entry->type, entry->length_in, entry->length_out,
				   entry->offset, entry->flags);

			kfn_extract(dumper, entry, entry->filename);
		}

		// Cleanup
		for (int i = 0; i < num_entries; i++) {
			free(entries[i].filename);
		}
		free(entries);
	}

	kfn_dumper_free(dumper);
	return 0;
}
