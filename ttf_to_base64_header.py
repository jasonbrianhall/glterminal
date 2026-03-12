#!/usr/bin/env python3
"""
Convert TTF font file to base64 C header
Usage: python3 ttf_to_base64.py font.ttf output.h
"""

import sys
import base64
import os

def ttf_to_base64_header(font_path, output_path):
    # Read the TTF file
    with open(font_path, 'rb') as f:
        ttf_data = f.read()
    
    # Get file info
    file_size = len(ttf_data)
    font_name = os.path.splitext(os.path.basename(font_path))[0]
    
    # Encode to base64
    b64_data = base64.b64encode(ttf_data).decode('ascii')
    
    # Split into 80-character lines for readability
    b64_lines = [b64_data[i:i+80] for i in range(0, len(b64_data), 80)]
    
    # Create header file
    header = f'''#ifndef MONOSPACE_H
#define MONOSPACE_H

#include <stddef.h>

// Embedded TTF font (base64 encoded)
// Original: {os.path.basename(font_path)}
// Original size: {file_size} bytes
// Base64 encoded size: {len(b64_data)} bytes
// 
// To decode: base64_decode(MONOSPACE_FONT_B64, MONOSPACE_FONT_B64_SIZE, &buffer, &decoded_size)
// Then use with FreeType: FT_New_Memory_Face(library, buffer, decoded_size, 0, &face)

static const char MONOSPACE_FONT_B64[] = 
'''
    
    # Add base64 data
    for i, line in enumerate(b64_lines):
        header += f'    "{line}"\n'
    
    header += f''';

static const size_t MONOSPACE_FONT_B64_SIZE = {len(b64_data)};
static const unsigned int MONOSPACE_FONT_ORIGINAL_SIZE = {file_size};

// Base64 decoder function
// Returns decoded data size, or -1 on error
// Caller must free the decoded buffer
static inline int base64_decode(const char *src, size_t src_len, unsigned char **out_data, size_t *out_len) {{
    // Allocate output buffer (base64 decodes to ~75% of input size)
    size_t max_out = (src_len * 3) / 4 + 10;
    *out_data = (unsigned char *)malloc(max_out);
    if (!*out_data) return -1;
    
    static const char base64_chars[] = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    unsigned char *dest = *out_data;
    size_t dest_pos = 0;
    int bits = 0;
    int bitcount = 0;
    
    for (size_t i = 0; i < src_len; i++) {{
        char c = src[i];
        
        // Skip whitespace
        if (c == '\\n' || c == '\\r' || c == ' ' || c == '\\t') continue;
        
        // Find value
        int val = -1;
        if (c >= 'A' && c <= 'Z') val = c - 'A';
        else if (c >= 'a' && c <= 'z') val = c - 'a' + 26;
        else if (c >= '0' && c <= '9') val = c - '0' + 52;
        else if (c == '+') val = 62;
        else if (c == '/') val = 63;
        else if (c == '=') break;  // Padding
        else continue;
        
        // Accumulate bits
        bits = (bits << 6) | val;
        bitcount += 6;
        
        // Output bytes when we have 8 bits
        if (bitcount >= 8) {{
            bitcount -= 8;
            dest[dest_pos++] = (bits >> bitcount) & 0xFF;
        }}
    }}
    
    *out_len = dest_pos;
    return 0;
}}

#endif // MONOSPACE_H
'''
    
    # Write header file
    with open(output_path, 'w') as f:
        f.write(header)
    
    print(f"✓ Created {output_path}")
    print(f"  Original TTF: {file_size:,} bytes")
    print(f"  Base64 data: {len(b64_data):,} bytes")
    print(f"  Header file: {len(header):,} bytes")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("Usage: python3 ttf_to_base64.py font.ttf output.h")
        sys.exit(1)
    
    ttf_to_base64_header(sys.argv[1], sys.argv[2])
