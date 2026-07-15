        function inflateRaw(data, outSize) {
            const out = new Uint8Array(outSize);
            let outPos = 0;
            let bytePos = 0, bitBuf = 0, bitCnt = 0;

            function getBit() {
                if (bitCnt === 0) {
                    bitBuf = data[bytePos++];
                    bitCnt = 8;
                }
                const bit = bitBuf & 1;
                bitBuf >>= 1;
                bitCnt--;
                return bit;
            }
            function getBits(n) {
                let val = 0;
                for (let i = 0; i < n; i++) val |= getBit() << i;
                return val >>> 0;
            }
            function alignByte() { bitCnt = 0; }

            function construct(lengths, n) {
                const counts = new Array(16).fill(0);
                for (let i = 0; i < n; i++) counts[lengths[i]]++;
                counts[0] = 0;
                const offs = new Array(16).fill(0);
                for (let i = 1; i < 16; i++) offs[i] = offs[i - 1] + counts[i - 1];
                const symbols = new Array(n).fill(0);
                for (let i = 0; i < n; i++) {
                    if (lengths[i] !== 0) symbols[offs[lengths[i]]++] = i;
                }
                return { counts, symbols };
            }

            function decodeSymbol(tree) {
                let code = 0, first = 0, index = 0;
                for (let len = 1; len < 16; len++) {
                    code |= getBit();
                    const count = tree.counts[len];
                    if (code - first < count) return tree.symbols[index + (code - first)];
                    index += count;
                    first += count;
                    first <<= 1;
                    code <<= 1;
                }
                throw new Error('invalid huffman code');
            }

            const LENGTH_BASE = [3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258];
            const LENGTH_EXTRA = [0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0];
            const DIST_BASE = [1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577];
            const DIST_EXTRA = [0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13];

            let fixedLitLen = null, fixedDist = null;
            function getFixedTrees() {
                if (fixedLitLen) return [fixedLitLen, fixedDist];
                const litLens = new Array(288);
                let i = 0;
                for (; i < 144; i++) litLens[i] = 8;
                for (; i < 256; i++) litLens[i] = 9;
                for (; i < 280; i++) litLens[i] = 7;
                for (; i < 288; i++) litLens[i] = 8;
                const distLens = new Array(30).fill(5);
                fixedLitLen = construct(litLens, 288);
                fixedDist = construct(distLens, 30);
                return [fixedLitLen, fixedDist];
            }

            function inflateBlockData(litLenTree, distTree) {
                while (true) {
                    const sym = decodeSymbol(litLenTree);
                    if (sym === 256) break;
                    if (sym < 256) {
                        out[outPos++] = sym;
                    } else {
                        const lenIdx = sym - 257;
                        const length = LENGTH_BASE[lenIdx] + getBits(LENGTH_EXTRA[lenIdx]);
                        const distSym = decodeSymbol(distTree);
                        const dist = DIST_BASE[distSym] + getBits(DIST_EXTRA[distSym]);
                        for (let i = 0; i < length; i++) {
                            out[outPos] = out[outPos - dist];
                            outPos++;
                        }
                    }
                }
            }

            const CLC_ORDER = [16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15];

            let final = 0;
            while (!final) {
                final = getBit();
                const type = getBits(2);
                if (type === 0) {
                    alignByte();
                    const len = data[bytePos] | (data[bytePos + 1] << 8);
                    bytePos += 4; // skip LEN + NLEN
                    for (let i = 0; i < len; i++) out[outPos++] = data[bytePos++];
                } else if (type === 1) {
                    const [lt, dt] = getFixedTrees();
                    inflateBlockData(lt, dt);
                } else if (type === 2) {
                    const hlit = getBits(5) + 257;
                    const hdist = getBits(5) + 1;
                    const hclen = getBits(4) + 4;
                    const clLens = new Array(19).fill(0);
                    for (let i = 0; i < hclen; i++) clLens[CLC_ORDER[i]] = getBits(3);
                    const clTree = construct(clLens, 19);

                    const lengths = new Array(hlit + hdist).fill(0);
                    let i = 0;
                    while (i < lengths.length) {
                        const sym = decodeSymbol(clTree);
                        if (sym < 16) {
                            lengths[i++] = sym;
                        } else if (sym === 16) {
                            const rep = getBits(2) + 3;
                            const prev = lengths[i - 1];
                            for (let r = 0; r < rep; r++) lengths[i++] = prev;
                        } else if (sym === 17) {
                            const rep = getBits(3) + 3;
                            for (let r = 0; r < rep; r++) lengths[i++] = 0;
                        } else {
                            const rep = getBits(7) + 11;
                            for (let r = 0; r < rep; r++) lengths[i++] = 0;
                        }
                    }
                    const litLenTree = construct(lengths.slice(0, hlit), hlit);
                    const distTree = construct(lengths.slice(hlit), hdist);
                    inflateBlockData(litLenTree, distTree);
                } else {
                    throw new Error('invalid deflate block type');
                }
            }

            return out;
        }

        function readUint16LE(buf, off) { return buf[off] | (buf[off + 1] << 8); }
        function readUint32LE(buf, off) {
            return (buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24)) >>> 0;
        }

        function parseZipEntries(bytes) {
            const EOCD_SIG = 0x06054b50;
            let eocdOffset = -1;
            const minOffset = Math.max(0, bytes.length - 65557);
            for (let i = bytes.length - 22; i >= minOffset; i--) {
                if (readUint32LE(bytes, i) === EOCD_SIG) { eocdOffset = i; break; }
            }
            if (eocdOffset === -1) throw new Error('Not a valid zip file (EOCD not found)');

            const totalEntries = readUint16LE(bytes, eocdOffset + 10);
            let cdOffset = readUint32LE(bytes, eocdOffset + 16);

            const entries = [];
            const CFH_SIG = 0x02014b50;
            for (let n = 0; n < totalEntries; n++) {
                if (readUint32LE(bytes, cdOffset) !== CFH_SIG) break;
                const compressionMethod = readUint16LE(bytes, cdOffset + 10);
                const compressedSize = readUint32LE(bytes, cdOffset + 20);
                const uncompressedSize = readUint32LE(bytes, cdOffset + 24);
                const nameLen = readUint16LE(bytes, cdOffset + 28);
                const extraLen = readUint16LE(bytes, cdOffset + 30);
                const commentLen = readUint16LE(bytes, cdOffset + 32);
                const localHeaderOffset = readUint32LE(bytes, cdOffset + 42);
                const nameBytes = bytes.subarray(cdOffset + 46, cdOffset + 46 + nameLen);
                const name = new TextDecoder().decode(nameBytes);

                entries.push({ name, compressionMethod, compressedSize, uncompressedSize, localHeaderOffset });
                cdOffset += 46 + nameLen + extraLen + commentLen;
            }
            return entries;
        }

        function extractZipEntry(bytes, entry) {
            const LFH_SIG = 0x04034b50;
            const off = entry.localHeaderOffset;
            if (readUint32LE(bytes, off) !== LFH_SIG) throw new Error('Invalid local file header');
            const nameLen = readUint16LE(bytes, off + 26);
            const extraLen = readUint16LE(bytes, off + 28);
            const dataStart = off + 30 + nameLen + extraLen;
            const compressed = bytes.subarray(dataStart, dataStart + entry.compressedSize);

            if (entry.compressionMethod === 0) {
                return compressed.slice();
            } else if (entry.compressionMethod === 8) {
                return inflateRaw(compressed, entry.uncompressedSize);
            } else {
                throw new Error('Unsupported zip compression method: ' + entry.compressionMethod);
            }
        }

        // Reads only entry names from a zip's central directory, given a possibly
        // truncated tail buffer plus how many bytes were trimmed off the front.
        function parseZipEntryNames(bytes, frontTrim) {
            const EOCD_SIG = 0x06054b50;
            let eocdOffset = -1;
            const minOffset = Math.max(0, bytes.length - 65557);
            for (let i = bytes.length - 22; i >= minOffset; i--) {
                if (readUint32LE(bytes, i) === EOCD_SIG) { eocdOffset = i; break; }
            }
            if (eocdOffset === -1) return null;

            const totalEntries = readUint16LE(bytes, eocdOffset + 10);
            let cdOffset = readUint32LE(bytes, eocdOffset + 16) - frontTrim;

            const names = [];
            const CFH_SIG = 0x02014b50;
            for (let n = 0; n < totalEntries; n++) {
                if (cdOffset < 0 || cdOffset + 46 > bytes.length) return null;
                if (readUint32LE(bytes, cdOffset) !== CFH_SIG) return null;
                const nameLen = readUint16LE(bytes, cdOffset + 28);
                const extraLen = readUint16LE(bytes, cdOffset + 30);
                const commentLen = readUint16LE(bytes, cdOffset + 32);
                names.push(new TextDecoder().decode(bytes.subarray(cdOffset + 46, cdOffset + 46 + nameLen)));
                cdOffset += 46 + nameLen + extraLen + commentLen;
            }
            return names;
        }

        // Cheaply checks whether a zip contains a .cdg file, using a ranged
        // request for just the tail of the file so we don't download the whole thing.
        async function peekZipHasCdg(url) {
            try {
                const resp = await fetch(url, { headers: { Range: 'bytes=-65536' } });
                if (!resp.ok) return null;
                const buf = new Uint8Array(await resp.arrayBuffer());

                let frontTrim = 0;
                if (resp.status === 206) {
                    const contentRange = resp.headers.get('Content-Range');
                    const match = contentRange && contentRange.match(/bytes (\d+)-(\d+)\/(\d+)/);
                    if (match) frontTrim = parseInt(match[1], 10);
                }

                const names = parseZipEntryNames(buf, frontTrim);
                if (names === null) return null;
                return names.some(n => n.toLowerCase().endsWith('.cdg'));
            } catch (err) {
                return null;
            }
        }

        // ---- music player ----