// CD+G parser/renderer, ported from cdg.cpp
const CDG_WIDTH = 300;
const CDG_HEIGHT = 216;

function cdgCreateState(bytes) {
    const packetCount = Math.floor(bytes.length / 24);
    const packets = new Array(packetCount);
    for (let i = 0; i < packetCount; i++) {
        const off = i * 24;
        packets[i] = {
            command: bytes[off] & 0x3F,
            instruction: bytes[off + 1] & 0x3F,
            data: bytes.subarray(off + 4, off + 20)
        };
    }
    return {
        packets,
        packetCount,
        currentPacket: 0,
        screen: new Uint8Array(CDG_WIDTH * CDG_HEIGHT),
        palette: new Array(16).fill(0x000000)
    };
}

function cdgReset(state) {
    state.screen.fill(0);
    state.currentPacket = 0;
}

function cdgUpdate(state, timeSeconds) {
    let target = Math.floor(timeSeconds * 300);
    if (target < 0) target = 0;
    if (target >= state.packetCount) target = state.packetCount - 1;
    if (target < state.currentPacket) cdgReset(state);
    while (state.currentPacket < target) {
        cdgProcessPacket(state, state.packets[state.currentPacket]);
        state.currentPacket++;
    }
}

function cdgProcessPacket(state, packet) {
    if ((packet.command & 0x3F) !== 0x09) return;
    const instruction = packet.instruction & 0x3F;
    const d = packet.data;

    if (instruction === 1) { // memory preset
        state.screen.fill(d[0] & 0x0F);
    } else if (instruction === 6 || instruction === 38) { // tile block (+ XOR)
        const xor = instruction === 38;
        const color0 = d[0] & 0x0F, color1 = d[1] & 0x0F;
        const row = d[2] & 0x1F, column = d[3] & 0x3F;
        const pixelRow = row * 12, pixelCol = column * 6;
        for (let y = 0; y < 12; y++) {
            const tileByte = d[4 + y];
            for (let x = 0; x < 6; x++) {
                const px = pixelCol + x, py = pixelRow + y;
                if (px >= 0 && px < CDG_WIDTH && py >= 0 && py < CDG_HEIGHT) {
                    const bit = (tileByte >> (5 - x)) & 1;
                    const color = bit ? color1 : color0;
                    const idx = py * CDG_WIDTH + px;
                    if (xor) state.screen[idx] ^= color; else state.screen[idx] = color;
                }
            }
        }
    } else if (instruction === 20 || instruction === 24) { // scroll preset / copy
        const color = d[0] & 0x0F;
        const hCmd = (d[1] & 0x30) >> 4;
        const vCmd = (d[2] & 0x30) >> 4;
        if (hCmd !== 0 || vCmd !== 0) {
            const wrap = instruction === 24;
            cdgScrollScreen(state, hCmd, vCmd, wrap ? -1 : color);
        }
    } else if (instruction === 30 || instruction === 31) { // load color table
        const offset = instruction === 30 ? 0 : 8;
        for (let i = 0; i < 8; i++) {
            const byte0 = d[2 * i] & 0x3F, byte1 = d[2 * i + 1] & 0x3F;
            const r = (byte0 >> 2) & 0x0F;
            const g = ((byte0 & 0x03) << 2) | ((byte1 >> 4) & 0x03);
            const b = byte1 & 0x0F;
            state.palette[offset + i] = ((r * 17) << 16) | ((g * 17) << 8) | (b * 17);
        }
    }
    // border preset (2) and transparency (28) are ignored, as in most players
}

function cdgScrollScreen(state, hCmd, vCmd, fillColor) {
    let hOffset = 0, vOffset = 0;
    if (hCmd === 1) hOffset = 6; else if (hCmd === 2) hOffset = -6;
    if (vCmd === 1) vOffset = 12; else if (vCmd === 2) vOffset = -12;
    if (hOffset === 0 && vOffset === 0) return;

    const temp = state.screen.slice();
    const wrap = fillColor === -1;
    for (let y = 0; y < CDG_HEIGHT; y++) {
        for (let x = 0; x < CDG_WIDTH; x++) {
            let srcX = x - hOffset, srcY = y - vOffset;
            if (wrap) {
                srcX = ((srcX % CDG_WIDTH) + CDG_WIDTH) % CDG_WIDTH;
                srcY = ((srcY % CDG_HEIGHT) + CDG_HEIGHT) % CDG_HEIGHT;
                state.screen[y * CDG_WIDTH + x] = temp[srcY * CDG_WIDTH + srcX];
            } else if (srcX >= 0 && srcX < CDG_WIDTH && srcY >= 0 && srcY < CDG_HEIGHT) {
                state.screen[y * CDG_WIDTH + x] = temp[srcY * CDG_WIDTH + srcX];
            } else {
                state.screen[y * CDG_WIDTH + x] = fillColor;
            }
        }
    }
}

function cdgRenderFrame(state) {
    const px = cdgImageData.data;
    for (let i = 0; i < state.screen.length; i++) {
        const rgb = state.palette[state.screen[i]];
        const o = i * 4;
        px[o] = (rgb >> 16) & 0xFF;
        px[o + 1] = (rgb >> 8) & 0xFF;
        px[o + 2] = rgb & 0xFF;
        px[o + 3] = 255;
    }
    cdgCtx.putImageData(cdgImageData, 0, 0);
}

function cdgLoop() {
    if (!cdgState) return;
    cdgUpdate(cdgState, musicAudio.currentTime);
    cdgRenderFrame(cdgState);
    cdgAnimHandle = requestAnimationFrame(cdgLoop);
}

function showCdgVisual() {
    musicArt.style.display = 'none';
    hideVizVisual();
    cdgCanvas.style.display = 'block';
    cdgExportWrap.style.display = 'block';
}

function hideCdgVisual() {
    cdgCanvas.style.display = 'none';
    cdgExportWrap.style.display = 'none';
    musicArt.style.display = 'flex';
}

function clearCdgState() {
    if (cdgAnimHandle) cancelAnimationFrame(cdgAnimHandle);
    cdgAnimHandle = null;
    cdgState = null;
    hideCdgVisual();
}

// Unzips a karaoke pack (matching .mp3 + .cdg inside a .zip) and returns
// a playable audio URL plus the raw CDG bytes, if present.
const AUDIO_MIME_TYPES = {
    mp3: 'audio/mpeg', wav: 'audio/wav', ogg: 'audio/ogg', m4a: 'audio/mp4',
    flac: 'audio/flac', aac: 'audio/aac', wma: 'audio/x-ms-wma', opus: 'audio/opus'
};

async function loadKaraokeZip(track) {
    const resp = await fetch(track.src);
    const buf = new Uint8Array(await resp.arrayBuffer());
    const entries = parseZipEntries(buf);

    let audioEntry = null;
    let cdgEntry = null;
    for (const entry of entries) {
        const lower = entry.name.toLowerCase();
        if (!audioEntry && AUDIO_EXTENSIONS.some(ext => lower.endsWith('.' + ext))) {
            audioEntry = entry;
        } else if (!cdgEntry && lower.endsWith('.cdg')) {
            cdgEntry = entry;
        }
    }

    if (!audioEntry) throw new Error('No audio file found in zip');

    const audioBytes = extractZipEntry(buf, audioEntry);
    const ext = audioEntry.name.slice(audioEntry.name.lastIndexOf('.') + 1).toLowerCase();
    const audioBlob = new Blob([audioBytes], { type: AUDIO_MIME_TYPES[ext] || 'application/octet-stream' });
    const audioUrl = URL.createObjectURL(audioBlob);
    const cdgBytes = cdgEntry ? extractZipEntry(buf, cdgEntry) : null;
    const displayName = audioEntry.name.split('/').pop();

    return { audioUrl, cdgBytes, displayName };
}

const musicBtn = document.getElementById('musicBtn');
const musicOverlay = document.getElementById('musicOverlay');
const musicAudio = document.getElementById('musicAudio');
const musicTitle = document.getElementById('musicTitle');
const musicSearchInput = document.getElementById('musicSearch');
const musicTable = document.getElementById('musicTable');
const musicTableBody = document.getElementById('musicTableBody');
const musicSeek = document.getElementById('musicSeek');
const musicTimeCurrent = document.getElementById('musicTimeCurrent');
const musicTimeDuration = document.getElementById('musicTimeDuration');
const musicPlayPause = document.getElementById('musicPlayPause');
const musicShuffleBtn = document.getElementById('musicShuffle');
const musicRepeatBtn = document.getElementById('musicRepeat');
const musicArt = document.getElementById('musicArt');

const cdgCanvas = document.getElementById('cdgCanvas');
const cdgCtx = cdgCanvas.getContext('2d');
const cdgExportWrap = document.getElementById('cdgExportWrap');
const cdgExportBtn = document.getElementById('cdgExportBtn');
let cdgExportRecorder = null;
let isCdgExporting = false;

cdgCanvas.style.cursor = 'pointer';
cdgCanvas.addEventListener('dblclick', () => {
    if (document.fullscreenElement) {
        document.exitFullscreen();
    } else {
        cdgCanvas.requestFullscreen();
    }
});
const cdgImageData = cdgCtx.createImageData(CDG_WIDTH, CDG_HEIGHT);

// ---- audio-reactive visualizer (Web Audio API), used when a track has no CD+G ----