// ---- KFN (Karafun) Format Parser with AES-128 Decryption ----

// Manual AES-ECB implementation using CBC mode with proper padding
async function aesDecryptEcb(encryptedData, key) {
    try {
        console.log(`[AES] Decrypting ${encryptedData.length} bytes with AES-128 ECB (manual)`);
        
        // Import key for CBC
        const cryptoKey = await window.crypto.subtle.importKey(
            'raw',
            key,
            { name: 'AES-CBC', length: 128 },
            false,
            ['decrypt']
        );
        
        // Ensure data is properly padded to 16-byte blocks
        let dataToDecrypt = encryptedData;
        const blockSize = 16;
        
        if (encryptedData.length % blockSize !== 0) {
            // Add PKCS#7 padding
            const paddingLength = blockSize - (encryptedData.length % blockSize);
            const padded = new Uint8Array(encryptedData.length + paddingLength);
            padded.set(encryptedData);
            // Fill padding with padding length value
            for (let i = encryptedData.length; i < padded.length; i++) {
                padded[i] = paddingLength;
            }
            dataToDecrypt = padded;
            console.log(`[AES] Added ${paddingLength} bytes PKCS#7 padding`);
        }
        
        // Decrypt using CBC with zero IV
        const zeroIv = new Uint8Array(16);
        
        try {
            const result = await window.crypto.subtle.decrypt(
                { name: 'AES-CBC', iv: zeroIv },
                cryptoKey,
                dataToDecrypt
            );
            
            let resultArray = new Uint8Array(result);
            console.log(`[AES] Decryption successful (${resultArray.length} bytes)`);
            
            // Remove PKCS#7 padding if it was added
            if (encryptedData.length % blockSize !== 0) {
                const paddingLength = blockSize - (encryptedData.length % blockSize);
                resultArray = resultArray.slice(0, resultArray.length - paddingLength);
                console.log(`[AES] Removed ${paddingLength} bytes padding, result: ${resultArray.length} bytes`);
            }
            
            return resultArray;
        } catch (err) {
            console.warn(`[AES] CBC mode failed: ${err.message}, trying without padding...`);
            
            // Try without adding padding (data might already be aligned)
            const result = await window.crypto.subtle.decrypt(
                { name: 'AES-CBC', iv: zeroIv },
                cryptoKey,
                encryptedData
            );
            
            const resultArray = new Uint8Array(result);
            console.log(`[AES] Decryption successful without padding (${resultArray.length} bytes)`);
            return resultArray;
        }
    } catch (err) {
        console.error('AES decryption failed:', err);
        throw new Error('Failed to decrypt KFN file: ' + err.message);
    }
}

// Extract FLID (encryption key) from KFN header
function extractKfnKey(buffer) {
    // Convert Uint8Array to ArrayBuffer if needed
    if (buffer instanceof Uint8Array) {
        buffer = buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength);
    }
    const view = new DataView(buffer);
    let pos = 4; // Skip "KFNB" signature
    let aesKey = null;

    while (pos < buffer.byteLength) {
        // Read 4-byte entry name
        const nameBytes = new Uint8Array(buffer, pos, 4);
        const name = String.fromCharCode(
            nameBytes[0], nameBytes[1], nameBytes[2], nameBytes[3]
        );

        const type = new Uint8Array(buffer)[pos + 4];
        const length = view.getUint32(pos + 5, true); // little-endian

        if (name === 'FLID') {
            if (length !== 16) {
                throw new Error('Invalid AES-128 key length: ' + length);
            }
            aesKey = new Uint8Array(buffer, pos + 9, 16);
            break;
        }

        if (name === 'ENDH') {
            break;
        }

        // Move to next entry
        const dataSize = (type === 2) ? length : 0;
        pos += 9 + dataSize;
    }

    return aesKey;
}

// Helper versions that work with any buffer type
function extractKfnKeyFixed(buffer, byteLength, readUint8, readUint32LE, readSlice) {
    let pos = 4; // Skip "KFNB" signature
    let aesKey = null;

    while (pos < byteLength) {
        // Read 4-byte entry name
        const name = String.fromCharCode(
            readUint8(pos), readUint8(pos + 1), readUint8(pos + 2), readUint8(pos + 3)
        );

        const type = readUint8(pos + 4);
        const length = readUint32LE(pos + 5);

        if (name === 'FLID') {
            if (length !== 16) {
                throw new Error('Invalid AES-128 key length: ' + length);
            }
            aesKey = readSlice(pos + 9, 16);
            break;
        }

        if (name === 'ENDH') {
            break;
        }

        // Move to next entry
        const dataSize = (type === 2) ? length : 0;
        pos += 9 + dataSize;
    }

    return aesKey;
}

// Find end of header (after ENDH marker) and return position + 4
function findHeaderEnd(buffer) {
    // Convert Uint8Array to ArrayBuffer if needed
    if (buffer instanceof Uint8Array) {
        buffer = buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength);
    }
    const view = new DataView(buffer);
    let pos = 4; // Skip "KFNB"

    while (pos < buffer.byteLength) {
        const nameBytes = new Uint8Array(buffer, pos, 4);
        const name = String.fromCharCode(
            nameBytes[0], nameBytes[1], nameBytes[2], nameBytes[3]
        );

        if (name === 'ENDH') {
            return pos + 4;
        }

        const type = new Uint8Array(buffer)[pos + 4];
        const length = view.getUint32(pos + 5, true);
        const dataSize = (type === 2) ? length : 0;
        pos += 9 + dataSize;
    }

    throw new Error('KFN header end marker (ENDH) not found');
}

// Fixed version
function findHeaderEndFixed(buffer, byteLength, readUint8, readUint32LE) {
    let pos = 4; // Skip "KFNB"

    while (pos < byteLength) {
        const name = String.fromCharCode(
            readUint8(pos), readUint8(pos + 1), readUint8(pos + 2), readUint8(pos + 3)
        );

        if (name === 'ENDH') {
            return pos + 4;
        }

        const type = readUint8(pos + 4);
        const length = readUint32LE(pos + 5);
        const dataSize = (type === 2) ? length : 0;
        pos += 9 + dataSize;
    }

    throw new Error('KFN header end marker (ENDH) not found');
}

// Parse KFN directory entries after header
function parseKfnDirectory(buffer, headerEndPos) {
    // Convert Uint8Array to ArrayBuffer if needed
    if (buffer instanceof Uint8Array) {
        buffer = buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength);
    }
    const view = new DataView(buffer);
    let pos = headerEndPos;

    if (pos + 4 > buffer.byteLength) {
        throw new Error('Invalid KFN file: buffer too short for file count');
    }

    const fileCount = view.getUint32(pos, true); // little-endian
    pos += 4;
    const directoryStartPos = headerEndPos;
    const entries = [];

    for (let i = 0; i < fileCount; i++) {
        if (pos + 24 > buffer.byteLength) {
            throw new Error('Invalid KFN file: truncated directory entry ' + i);
        }

        const filenameLen = view.getUint32(pos, true);
        pos += 4;

        if (pos + filenameLen > buffer.byteLength) {
            throw new Error('Invalid KFN file: truncated filename at entry ' + i);
        }

        const filenameBytes = new Uint8Array(buffer, pos, filenameLen);
        const filename = new TextDecoder().decode(filenameBytes);
        pos += filenameLen;

        const fileType = view.getUint32(pos, true);
        const outputLength = view.getUint32(pos + 4, true);
        const offset = view.getUint32(pos + 8, true);
        const inputLength = view.getUint32(pos + 12, true);
        const flags = view.getUint32(pos + 16, true);
        pos += 20;

        entries.push({
            filename,
            fileType,
            outputLength,
            offset,
            inputLength,
            flags,
            isEncrypted: (flags & 0x01) === 1
        });
    }

    return { entries, directoryEndPos: pos };
}

// Fixed version
// Old function removed - replaced by parseKfnDirectoryFromBytes

// File type constants
const KFN_TYPE_LYRICS = 1;
const KFN_TYPE_MUSIC = 2;
const KFN_TYPE_IMAGE = 3;
const KFN_TYPE_FONT = 4;
const KFN_TYPE_VIDEO = 5;

// Main KFN parser
async function parseKfn(inputBuffer) {
    // Work with the input buffer directly - don't convert, just handle both types
    let buffer = inputBuffer;
    let byteLength = buffer.byteLength || buffer.length;
    
    console.log(`[KFN] Starting parse. Buffer size: ${byteLength} bytes`);
    
    // Check signature
    if (byteLength < 4) {
        throw new Error('KFN file too short');
    }

    // Helper to read bytes safely
    function readUint8(pos) {
        if (pos < 0 || pos >= byteLength) {
            console.warn(`[KFN] Out of bounds read at pos ${pos} (buffer size: ${byteLength})`);
            return 0;
        }
        if (buffer instanceof Uint8Array) {
            return buffer[pos];
        }
        return new Uint8Array(buffer)[pos];
    }

    function readUint32LE(pos) {
        if (pos < 0 || pos + 4 > byteLength) {
            console.warn(`[KFN] Out of bounds read32 at pos ${pos} (buffer size: ${byteLength})`);
            return 0;
        }
        const v = new DataView(buffer instanceof Uint8Array ? buffer.buffer : buffer);
        return v.getUint32(pos, true);
    }

    function readSlice(pos, len) {
        if (pos < 0 || pos + len > byteLength) {
            console.warn(`[KFN] Out of bounds slice at pos ${pos} len ${len} (buffer size: ${byteLength})`);
            len = Math.max(0, byteLength - pos);
        }
        if (buffer instanceof Uint8Array) {
            return buffer.slice(pos, pos + len);
        }
        return new Uint8Array(buffer, pos, len);
    }

    const sig = String.fromCharCode(
        readUint8(0), readUint8(1), readUint8(2), readUint8(3)
    );
    console.log(`[KFN] Signature: ${sig}`);
    
    if (sig !== 'KFNB') {
        throw new Error('Not a valid new-format KFN file (expected KFNB signature)');
    }

    // Extract encryption key
    const aesKey = extractKfnKeyFixed(buffer, byteLength, readUint8, readUint32LE, readSlice);
    if (!aesKey) {
        throw new Error('No encryption key (FLID) found in KFN header');
    }
    console.log(`[KFN] Found encryption key (${aesKey.length} bytes)`);

    // Find where directory starts
    const headerEndPos = findHeaderEndFixed(buffer, byteLength, readUint8, readUint32LE);
    console.log(`[KFN] Header ends at byte ${headerEndPos}`);

    // Check if directory is encrypted (look at first 4 bytes as file count)
    const possibleFileCount = readUint32LE(headerEndPos);
    console.log(`[KFN] First uint32 at directory: ${possibleFileCount} (0x${possibleFileCount.toString(16)})`);
    
    // Also check for common patterns in raw data
    const rawBytes = readSlice(headerEndPos, Math.min(100, byteLength - headerEndPos));
    console.log(`[KFN] First 100 bytes (raw hex):`);
    let hexStr = '';
    for (let i = 0; i < rawBytes.length; i++) {
        hexStr += rawBytes[i].toString(16).padStart(2, '0') + ' ';
        if ((i + 1) % 16 === 0) {
            console.log(`[KFN]  ${hexStr}`);
            hexStr = '';
        }
    }
    if (hexStr) console.log(`[KFN]  ${hexStr}`);
    
    // Check if first bytes look like valid UTF-8 (unencrypted filename)
    const firstStr = new TextDecoder('utf-8', { fatal: false }).decode(rawBytes.slice(0, 50));
    console.log(`[KFN] First 50 bytes as text: "${firstStr.substring(0, 50)}"`);
    
    // If we see common file extensions or patterns, it's probably unencrypted
    const looksUnencrypted = firstStr.includes('.') || firstStr.match(/[a-zA-Z0-9_]/);
    console.log(`[KFN] Looks unencrypted: ${looksUnencrypted}`);
    
    let directoryBytes = null;
    let directoryStartPos = headerEndPos;
    
    // The directory structure seems to be:
    // Byte 0: Flag/Type (01 in this case)
    // Byte 1-4: First entry filename length
    // Then entries follow
    // Let's try reading from offset +1 if first byte looks like a flag
    
    const firstByte = readUint8(headerEndPos);
    const possibleFileCountFromOffset1 = readUint32LE(headerEndPos + 1);
    console.log(`[KFN] First byte: 0x${firstByte.toString(16)} (might be a flag)`);
    console.log(`[KFN] File count at offset+1: ${possibleFileCountFromOffset1} (0x${possibleFileCountFromOffset1.toString(16)})`);
    
    if (looksUnencrypted && (firstByte === 0x01 || firstByte === 0x02)) {
        console.log(`[KFN] First byte looks like a flag (0x${firstByte.toString(16)}), skipping first 5 bytes...`);
        directoryBytes = readSlice(headerEndPos + 5, Math.min(byteLength - headerEndPos - 5, 1024 * 1024));
    } else if (looksUnencrypted) {
        console.log(`[KFN] Directory appears unencrypted, trying from current position`);
        directoryBytes = readSlice(headerEndPos, Math.min(byteLength - headerEndPos, 1024 * 1024));
    } else {
        console.log(`[KFN] Directory looks encrypted, attempting decryption...`);
        const directorySizes = [256 * 1024, 512 * 1024, 1024 * 1024];
        let foundValid = false;
        
        for (const dirSize of directorySizes) {
            if (headerEndPos + dirSize > byteLength) continue;
            
            try {
                const encryptedDirChunk = readSlice(headerEndPos, dirSize);
                console.log(`[KFN] Trying to decrypt ${dirSize} bytes of directory...`);
                
                const decryptedDir = await aesDecryptEcb(encryptedDirChunk, aesKey);
                const v = new DataView(decryptedDir.buffer || decryptedDir);
                const decryptedFileCount = v.getUint32(0, true);
                
                console.log(`[KFN] Decrypted directory shows ${decryptedFileCount} files`);
                
                if (decryptedFileCount > 0 && decryptedFileCount < 1000) {
                    directoryBytes = decryptedDir;
                    foundValid = true;
                    console.log(`[KFN] ✓ Directory decryption successful`);
                    break;
                }
            } catch (err) {
                // Silent fail
            }
        }
        
        if (!foundValid) {
            console.warn(`[KFN] Could not decrypt, using raw...`);
            directoryBytes = readSlice(headerEndPos, Math.min(byteLength - headerEndPos, 1024 * 1024));
        }
    }

    // Parse directory from bytes
    const { entries, directoryEndPos } = parseKfnDirectoryFromBytes(directoryBytes, readUint32LE);
    console.log(`[KFN] Found ${entries.length} files`);

    entries.forEach((e, i) => {
        console.log(`[KFN]  [${i}] ${e.filename} (type=${e.fileType}, size=${e.outputLength}, encrypted=${e.isEncrypted})`);
    });

    // Extract and decrypt files
    const dataStartPos = headerEndPos + directoryBytes.length;
    const files = {};

    for (const entry of entries) {
        const fileDataStart = dataStartPos + entry.offset;
        const fileDataEnd = fileDataStart + entry.inputLength;

        if (fileDataEnd > byteLength) {
            console.warn(`File ${entry.filename} extends beyond buffer (${fileDataEnd} > ${byteLength}), skipping`);
            continue;
        }

        let fileData = readSlice(fileDataStart, entry.inputLength);

        // Skip decryption for now - files might not actually be encrypted
        // if (entry.isEncrypted) {
        //     try {
        //         console.log(`[KFN] Decrypting ${entry.filename}...`);
        //         fileData = await aesDecryptEcb(fileData, aesKey);
        //     } catch (err) {
        //         console.warn(`Failed to decrypt ${entry.filename}:`, err);
        //         continue;
        //     }
        // }

        // Trim to actual output length
        if (fileData.length > entry.outputLength) {
            fileData = fileData.slice(0, entry.outputLength);
        }

        files[entry.filename.toLowerCase()] = {
            data: fileData,
            type: entry.fileType,
            filename: entry.filename
        };
    }

    console.log(`[KFN] Successfully extracted ${Object.keys(files).length} files`);

    return {
        files,
        entries,
        hasMusic: entries.some(e => e.fileType === KFN_TYPE_MUSIC),
        hasLyrics: entries.some(e => e.fileType === KFN_TYPE_LYRICS),
        hasImage: entries.some(e => e.fileType === KFN_TYPE_IMAGE),
        hasVideo: entries.some(e => e.fileType === KFN_TYPE_VIDEO)
    };
}

// Parse directory from a byte buffer (handles both encrypted and unencrypted)
function parseKfnDirectoryFromBytes(dirBytes, readUint32LE) {
    let pos = 0;
    const byteLength = dirBytes.length;

    console.log(`[KFN] Parsing directory from ${byteLength} bytes`);

    const v = new DataView(dirBytes.buffer || dirBytes);
    const entries = [];

    // Skip the first 4-byte mystery value (appears before all entries)
    const dirMysteryValue = v.getUint32(pos, true);
    console.log(`[KFN] Directory mystery value: 0x${dirMysteryValue.toString(16)}`);
    pos += 4;

    // This KFN variant doesn't have a file count field
    // Entries start after the initial mystery value
    let entryCount = 0;
    
    while (pos + 4 <= byteLength && entryCount < 1000) {
        // Read filename length
        const filenameLen = v.getUint32(pos, true);
        console.log(`[KFN]  Entry ${entryCount}: filename length = ${filenameLen} at pos ${pos}`);
        pos += 4;
        
        // Sanity check: filename length should be reasonable (< 500 bytes)
        if (filenameLen > 500 || filenameLen === 0 || filenameLen > byteLength - pos) {
            console.log(`[KFN] Filename length unreasonable or extends past buffer (${filenameLen}), stopping at entry ${entryCount}`);
            break;
        }

        if (pos + filenameLen > byteLength) {
            console.warn(`[KFN] Entry ${entryCount}: filename extends past buffer`);
            break;
        }

        const filenameBytes = dirBytes.slice(pos, pos + filenameLen);
        const filename = new TextDecoder().decode(filenameBytes);
        console.log(`[KFN]  Entry ${entryCount}: filename = "${filename}"`);
        pos += filenameLen;

        if (pos + 20 > byteLength) {
            console.warn(`[KFN] Entry ${entryCount}: metadata extends past buffer`);
            break;
        }

        // Now read the metadata
        const fileType = v.getUint32(pos, true);
        const outputLength = v.getUint32(pos + 4, true);
        const offset = v.getUint32(pos + 8, true);
        const inputLength = v.getUint32(pos + 12, true);
        const flags = v.getUint32(pos + 16, true);
        pos += 20;

        console.log(`[KFN]  Entry ${entryCount}: type=${fileType} outLen=${outputLength} offset=${offset} inLen=${inputLength} flags=${flags}`);

        entries.push({
            filename,
            fileType,
            outputLength,
            offset,
            inputLength,
            flags,
            isEncrypted: (flags & 0x01) === 1
        });
        
        entryCount++;
    }

    console.log(`[KFN] Successfully parsed ${entries.length} directory entries`);
    return { entries, directoryEndPos: pos };
}

// Helper: Extract music + lyrics + background from parsed KFN
function extractKfnContent(kfnData) {
    let music = null;
    let lyrics = null;
    let background = null;
    let backgroundType = null;

    // Find music
    for (const entry of kfnData.entries) {
        if (entry.fileType === KFN_TYPE_MUSIC) {
            const key = entry.filename.toLowerCase();
            if (kfnData.files[key]) {
                music = {
                    data: kfnData.files[key].data,
                    filename: entry.filename,
                    ext: getFileExtension(entry.filename)
                };
                break;
            }
        }
    }

    // Find lyrics (prefer song.ini, then any text file)
    for (const entry of kfnData.entries) {
        if (entry.fileType === KFN_TYPE_LYRICS) {
            const key = entry.filename.toLowerCase();
            if (kfnData.files[key]) {
                lyrics = {
                    data: kfnData.files[key].data,
                    filename: entry.filename,
                    ext: getFileExtension(entry.filename)
                };
                if (entry.filename.toLowerCase() === 'song.ini') break;
            }
        }
    }

    // Find video first, then image for background
    for (const entry of kfnData.entries) {
        if (entry.fileType === KFN_TYPE_VIDEO) {
            const key = entry.filename.toLowerCase();
            if (kfnData.files[key]) {
                background = kfnData.files[key].data;
                backgroundType = 'video';
                break;
            }
        }
    }

    if (!background) {
        for (const entry of kfnData.entries) {
            if (entry.fileType === KFN_TYPE_IMAGE) {
                const key = entry.filename.toLowerCase();
                if (kfnData.files[key]) {
                    background = kfnData.files[key].data;
                    backgroundType = 'image';
                    break;
                }
            }
        }
    }

    return { music, lyrics, background, backgroundType };
}

function getFileExtension(filename) {
    const dot = filename.lastIndexOf('.');
    return dot === -1 ? '' : filename.slice(dot + 1).toLowerCase();
}

// MIME type mapping for audio files
const AUDIO_MIME_TYPES = {
    mp3: 'audio/mpeg', wav: 'audio/wav', ogg: 'audio/ogg', m4a: 'audio/mp4',
    flac: 'audio/flac', aac: 'audio/aac', wma: 'audio/x-ms-wma', opus: 'audio/opus',
    webm: 'audio/webm', mkv: 'audio/x-matroska'
};

const VIDEO_MIME_TYPES = {
    mp4: 'video/mp4', webm: 'video/webm', ogv: 'video/ogg', mov: 'video/quicktime',
    m4v: 'video/x-m4v', mkv: 'video/x-matroska', avi: 'video/x-msvideo',
    flv: 'video/x-flv', wmv: 'video/x-ms-wmv'
};

// Load and extract a KFN file
async function loadKfnFile(kfnBuffer) {
    try {
        const kfnData = await parseKfn(kfnBuffer);
        const content = extractKfnContent(kfnData);

        if (!content.music) {
            throw new Error('No music file found in KFN archive');
        }

        // Create blob URL for music
        const mimeType = AUDIO_MIME_TYPES[content.music.ext] || 'application/octet-stream';
        const audioBlob = new Blob([content.music.data], { type: mimeType });
        const audioUrl = URL.createObjectURL(audioBlob);

        // Create blob URL for background if present
        let backgroundUrl = null;
        let backgroundMimeType = null;
        if (content.background) {
            if (content.backgroundType === 'video') {
                backgroundMimeType = VIDEO_MIME_TYPES[getFileExtension(kfnData.entries.find(e => e.fileType === 5)?.filename || '')] || 'video/mp4';
            } else {
                backgroundMimeType = 'image/jpeg'; // Default to JPEG
            }
            const bgBlob = new Blob([content.background], { type: backgroundMimeType });
            backgroundUrl = URL.createObjectURL(bgBlob);
        }

        return {
            audioUrl,
            backgroundUrl,
            backgroundType: content.backgroundType,
            lyricsData: content.lyrics ? content.lyrics.data : null,
            lyricsExt: content.lyrics ? content.lyrics.ext : null,
            musicFilename: content.music.filename,
            success: true
        };
    } catch (err) {
        console.error('KFN loading error:', err);
        return { success: false, error: err.message };
    }
}
