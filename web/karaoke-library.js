/**
 * KaraokePlayer Library
 * A reusable karaoke player with lyrics sync, multiple audio tracks, and playback control
 */

const ENTRY_TYPES = {
    SONGTEXT: 1,
    MUSIC: 2,
    IMAGE: 3,
    FONT: 4,
    VIDEO: 5
};

class KFNDumper {
    constructor(arrayBuffer) {
        this.buffer = new Uint8Array(arrayBuffer);
        this.offset = 0;
        this.aesKey = null;
    }

    readByte() {
        if (this.offset >= this.buffer.length) {
            throw new Error('Unexpected end of file');
        }
        return this.buffer[this.offset++];
    }

    readDword() {
        const b1 = this.readByte();
        const b2 = this.readByte();
        const b3 = this.readByte();
        const b4 = this.readByte();
        return b1 | (b2 << 8) | (b3 << 16) | (b4 << 24);
    }

    readBytes(length) {
        if (this.offset + length > this.buffer.length) {
            throw new Error(`Cannot read ${length} bytes: not enough data`);
        }
        const data = this.buffer.slice(this.offset, this.offset + length);
        this.offset += length;
        return data;
    }

    readString(length) {
        const bytes = this.readBytes(length);
        const decoder = new TextDecoder('utf-8');
        return decoder.decode(bytes);
    }

    list() {
        this.offset = 0;
        const entries = [];

        const sig = this.readString(4);
        if (sig !== 'KFNB') {
            throw new Error('Invalid KFN file signature');
        }

        while (true) {
            const headerSig = this.readString(4);
            const type = this.readByte();
            const lenOrValue = this.readDword();
            let buf = null;

            if (type === 2) {
                buf = this.readBytes(lenOrValue);
            }

            if (headerSig === 'FLID' && buf !== null) {
                this.aesKey = buf;
            }

            if (headerSig === 'ENDH') {
                break;
            }
        }

        const numFiles = this.readDword();

        for (let i = 0; i < numFiles; i++) {
            const filenameLen = this.readDword();
            const filename = this.readString(filenameLen);

            const entry = {
                filename,
                type: this.readDword(),
                lengthOut: this.readDword(),
                offset: this.readDword(),
                lengthIn: this.readDword(),
                flags: this.readDword()
            };

            entries.push(entry);
        }

        const dirEnd = this.offset;
        entries.forEach(entry => {
            entry.offset += dirEnd;
        });

        return entries;
    }

    async extract(entry) {
        if ((entry.flags & 0x01) && !this.aesKey) {
            throw new Error('Encryption key is unknown');
        }

        const data = this.buffer.slice(entry.offset, entry.offset + entry.lengthIn);

        if (entry.flags & 0x01) {
            return await this.decryptAES128ECB(data, this.aesKey, entry.lengthOut);
        }

        return data.slice(0, entry.lengthOut);
    }

    async decryptAES128ECB(encryptedData, key, outputLength) {
        if (!window.CryptoJS) {
            throw new Error('CryptoJS library not loaded');
        }

        const keyWordArray = window.CryptoJS.enc.Hex.parse(
            Array.from(key).map(b => b.toString(16).padStart(2, '0')).join('')
        );
        
        const encryptedWordArray = window.CryptoJS.enc.Hex.parse(
            Array.from(encryptedData).map(b => b.toString(16).padStart(2, '0')).join('')
        );

        const decrypted = window.CryptoJS.AES.decrypt(
            { ciphertext: encryptedWordArray },
            keyWordArray,
            { mode: window.CryptoJS.mode.ECB, padding: window.CryptoJS.pad.NoPadding }
        );

        const decryptedBytes = new Uint8Array(decrypted.sigBytes);
        for (let i = 0; i < decrypted.sigBytes; i++) {
            decryptedBytes[i] = (decrypted.words[i >>> 2] >>> (24 - (i % 4) * 8)) & 0xff;
        }

        return decryptedBytes.slice(0, outputLength);
    }

    async extractAll() {
        const entries = this.list();
        const result = {};

        for (const entry of entries) {
            try {
                result[entry.filename] = {
                    data: await this.extract(entry),
                    type: entry.type,
                    typeName: Object.keys(ENTRY_TYPES).find(
                        key => ENTRY_TYPES[key] === entry.type
                    ) || 'UNKNOWN',
                    encrypted: (entry.flags & 0x01) !== 0,
                    metadata: entry
                };
            } catch (error) {
                result[entry.filename] = {
                    error: error.message,
                    metadata: entry
                };
            }
        }

        return result;
    }
}

// Pulls the audio filename out of a comma-separated ini value like
// "1,I,instru.ogg" or "ld.ogg,0,0,," — the filename can be in any field
// position depending on KFN version, so match by extension rather than
// assuming a fixed column.
function extractAudioFilename(value) {
    const parts = value.split(',').map(p => p.trim());
    for (const part of parts) {
        if (/\.(mp3|ogg|wav|m4a|aac|flac|wma)$/i.test(part)) return part;
    }
    return null;
}

class KaraokePlayer {
    constructor(config = {}) {
        this.audioVocal = null;
        this.audioBacking = null;
        this.lyrics = [];
        this.lyricsLines = [];
        this.syncTimes = [];
        this.lineIndices = [];
        this.currentLineIndex = 0;
        this.syncOffset = 0;
        this.autoAdvance = false;
        this.vocalMuted = false;
        this.backingMuted = false;
        // Two independent <audio> elements never stay perfectly locked
        // together, so drift beyond this (seconds) triggers a resync.
        this.DRIFT_TOLERANCE = 0.08;
        // Track filenames as declared by Song.ini (see parseINI) — used to
        // assign vocal vs. backing with certainty instead of guessing.
        this.backingFilename = null;
        this.vocalFilenames = [];
        
        this.callbacks = {
            onLyricsUpdate: config.onLyricsUpdate || (() => {}),
            onPlaybackStateChange: config.onPlaybackStateChange || (() => {}),
            onMetadataLoaded: config.onMetadataLoaded || (() => {}),
            onAudioLoaded: config.onAudioLoaded || (() => {}),
            onPlaybackEnd: config.onPlaybackEnd || (() => {}),
            onSyncOffsetChange: config.onSyncOffsetChange || (() => {})
        };
    }

    // File parsing
    async parseKFN(arrayBuffer) {
        const dumper = new KFNDumper(arrayBuffer);
        const extracted = await dumper.extractAll();

        console.log('Extracted files:', Object.keys(extracted));
        for (const [filename, info] of Object.entries(extracted)) {
            console.log(`${filename}:`, info.error || `${info.data.length} bytes`);
        }

        let songIniData = null;
        const audioData = [];

        for (const [filename, info] of Object.entries(extracted)) {
            if (!info.error) {
                if (filename.toLowerCase() === 'song.ini') {
                    songIniData = info.data;
                } else if (/\.(mp3|ogg|wav|m4a|aac)$/i.test(filename)) {
                    audioData.push({ filename, data: info.data });
                }
            }
        }

        if (songIniData) {
            const decoder = new TextDecoder('utf-8');
            const iniContent = decoder.decode(songIniData);
            console.log('INI Content:', iniContent);
            this.parseINI(iniContent);
        }

        audioData.forEach((audio) => {
            this.loadAudioBlob(audio.data, audio.filename);
        });

        console.log('Parsed lyrics:', this.lyrics.length, 'Sync times:', this.syncTimes.length);
        return { success: true, audioCount: audioData.length };
    }

    parseINI(content) {
        console.log('parseINI called with content length:', content.length);
        
        const lines = content.split(/\r?\n/);
        let title = 'Unknown Song';
        let artist = 'Unknown Artist';
        const textMap = {};
        const syncMap = {};
        let currentSection = '';
        let backingFilename = null;
        const vocalFilenames = [];

        for (let line of lines) {
            line = line.trim();
            
            if (line.startsWith('[') && line.endsWith(']')) {
                currentSection = line.slice(1, -1).toLowerCase();
                continue;
            }
            
            const match = line.match(/^([^=]+)=(.*)$/);
            if (!match) continue;
            const [, key, value] = match;
            const keyLower = key.toLowerCase();

            if (currentSection === 'general' || currentSection === '') {
                if (keyLower === 'title') title = value;
                if (keyLower === 'artist') artist = value;
                // e.g. "source=1,I,instru.ogg" — the "I" flags this as the
                // instrumental/backing track; the filename is authoritative.
                if (keyLower === 'source') {
                    backingFilename = extractAudioFilename(value);
                }
            }

            // [mp3music] track0=ld.ogg,0,0,, — additional tracks (guide
            // vocal, lead, etc). Anything listed here that isn't the
            // declared backing track is treated as vocal.
            if (currentSection === 'mp3music' && /^track\d+$/.test(keyLower)) {
                const f = extractAudioFilename(value);
                if (f) vocalFilenames.push(f);
            }
            
            if (key.match(/^text\d+$/)) {
                const idx = parseInt(key.match(/\d+/)[0]);
                textMap[idx] = value;
            }
            if (key.match(/^sync\d+$/)) {
                const idx = parseInt(key.match(/\d+/)[0]);
                syncMap[idx] = value.split(',').map(v => parseInt(v) * 10);
            }
        }

        this.backingFilename = backingFilename ? backingFilename.toLowerCase() : null;
        this.vocalFilenames = vocalFilenames
            .filter(f => f.toLowerCase() !== this.backingFilename)
            .map(f => f.toLowerCase());

        console.log('Backing file (from ini):', this.backingFilename);
        console.log('Vocal file(s) (from ini):', this.vocalFilenames);

        console.log('Text entries:', Object.keys(textMap).length, 'Sync entries:', Object.keys(syncMap).length);
        console.log('Title:', title, 'Artist:', artist);

        this.syncTimes = [];
        this.lyrics = [];
        this.lyricsLines = [];
        this.lineIndices = [];
        
        const allTimings = [];
        for (let i = 0; syncMap[i] !== undefined; i++) {
            allTimings.push(...syncMap[i]);
        }
        
        console.log('Total timings:', allTimings.length);
        
        let timingIdx = 0;
        for (let i = 0; textMap[i] !== undefined; i++) {
            const line = textMap[i];
            const syllables = line.split('/').filter(s => s.length > 0);
            const words = [];
            
            for (let syl of syllables) {
                const parts = syl.split(/\s+/).filter(p => p.length > 0);
                words.push(...parts);
            }
            
            if (words.length > 0) {
                this.lineIndices.push(this.lyrics.length);
                this.lyricsLines.push(line.replace(/\//g, ''));
                
                for (let j = 0; j < words.length; j++) {
                    this.lyrics.push(words[j]);
                    this.syncTimes.push(allTimings[timingIdx] !== undefined ? allTimings[timingIdx] : 0);
                    timingIdx++;
                }
            }
        }

        console.log('Final lyrics:', this.lyrics.length, 'Lines:', this.lyricsLines.length);

        this.callbacks.onMetadataLoaded({ title, artist });
        return { title, artist };
    }

    // Audio handling
    loadAudioBlob(blob, filename, index) {
        const url = URL.createObjectURL(new Blob([blob]));
        this.createAudioElement(url, filename, index);
    }

    createAudioElement(url, filename, index) {
        const audio = new Audio(url);
        const base = filename.toLowerCase().split(/[\\/]/).pop();

        // Song.ini declares each track's role explicitly — use that
        // instead of guessing from the filename.
        let isVocal;
        if (this.backingFilename && base === this.backingFilename) {
            isVocal = false;
        } else if (this.vocalFilenames.includes(base)) {
            isVocal = true;
        } else if (!this.audioVocal && !this.audioBacking) {
            // Ini didn't mention this file (or wasn't present) — fall back
            // to filling whichever slot is open, vocal first.
            isVocal = true;
        } else if (!this.audioVocal) {
            isVocal = true;
        } else if (!this.audioBacking) {
            isVocal = false;
        } else {
            return; // both slots already filled
        }

        audio.addEventListener('loadedmetadata', () => {
            this.callbacks.onAudioLoaded({
                duration: audio.duration,
                isVocal,
                filename
            });
        });

        audio.addEventListener('timeupdate', () => this.updateSync());
        audio.addEventListener('ended', () => {
            this.autoAdvance = false;
            this.updateLyricsDisplay();
            this.callbacks.onPlaybackEnd();
        });

        if (isVocal) {
            this.audioVocal = audio;
        } else {
            this.audioBacking = audio;
        }
    }

    // Playback control
    play() {
        const isPlaying = (this.audioVocal && !this.audioVocal.paused) || (this.audioBacking && !this.audioBacking.paused);

        if (!isPlaying) {
            if (this.audioVocal && !this.vocalMuted) this.audioVocal.play();
            if (this.audioBacking && !this.backingMuted) this.audioBacking.play();
            this.autoAdvance = true;
            this.callbacks.onPlaybackStateChange({ isPlaying: true });
        }
    }

    pause() {
        if (this.audioVocal) this.audioVocal.pause();
        if (this.audioBacking) this.audioBacking.pause();
        this.callbacks.onPlaybackStateChange({ isPlaying: false });
    }

    toggle() {
        const isPlaying = (this.audioVocal && !this.audioVocal.paused) || (this.audioBacking && !this.audioBacking.paused);

        if (isPlaying) {
            this.pause();
        } else {
            this.play();
        }
    }

    reset() {
        if (this.audioVocal) this.audioVocal.currentTime = 0;
        if (this.audioBacking) this.audioBacking.currentTime = 0;
        this.currentLineIndex = 0;
        this.autoAdvance = false;
        this.updateLyricsDisplay();
    }

    seek(time) {
        if (this.audioVocal) this.audioVocal.currentTime = time;
        if (this.audioBacking) this.audioBacking.currentTime = time;
        this.updateSync();
    }

    seekByPercent(percent) {
        const audio = this.audioVocal || this.audioBacking;
        if (!audio) return;
        const time = percent * audio.duration;
        this.seek(time);
    }

    // Volume/muting
    toggleVocal() {
        this.vocalMuted = !this.vocalMuted;
        if (this.audioVocal) {
            if (this.vocalMuted) {
                this.audioVocal.pause();
            } else {
                if (this.audioBacking && !this.audioBacking.paused) {
                    this.audioVocal.currentTime = this.audioBacking.currentTime;
                }
                if ((this.audioVocal && !this.audioVocal.paused) || 
                    (this.audioBacking && !this.audioBacking.paused) || 
                    this.autoAdvance) {
                    this.audioVocal.play();
                }
            }
        }
        return this.vocalMuted;
    }

    toggleBacking() {
        this.backingMuted = !this.backingMuted;
        if (this.audioBacking) {
            if (this.backingMuted) {
                this.audioBacking.pause();
            } else {
                if (this.audioVocal && !this.audioVocal.paused) {
                    this.audioBacking.currentTime = this.audioVocal.currentTime;
                }
                if ((this.audioVocal && !this.audioVocal.paused) || 
                    (this.audioBacking && !this.audioBacking.paused) || 
                    this.autoAdvance) {
                    this.audioBacking.play();
                }
            }
        }
        return this.backingMuted;
    }

    setVocalVolume(volume) {
        if (this.audioVocal) this.audioVocal.volume = Math.max(0, Math.min(1, volume));
    }

    setBackingVolume(volume) {
        if (this.audioBacking) this.audioBacking.volume = Math.max(0, Math.min(1, volume));
    }

    // Sync
    setSyncOffset(offset) {
        this.syncOffset = parseInt(offset);
        this.callbacks.onSyncOffsetChange({ offset: this.syncOffset });
    }

    updateSync() {
        // Use whichever audio is playing for sync timing
        let time = null;
        if (this.audioVocal && !this.audioVocal.paused) {
            time = this.audioVocal;
        } else if (this.audioBacking && !this.audioBacking.paused) {
            time = this.audioBacking;
        }
        if (!time) return;

        this.correctDrift();

        const currentMs = time.currentTime * 1000;

        if (this.autoAdvance && this.lyrics.length > 0) {
            // Search from the beginning to find the correct lyric index
            let nextIdx = 0;
            for (let i = 0; i < this.syncTimes.length; i++) {
                if (currentMs >= (this.syncTimes[i] + this.syncOffset)) {
                    nextIdx = i;
                } else {
                    break;
                }
            }
            this.currentLineIndex = nextIdx;
        }

        // Always update display to keep progress bar moving and lyrics in sync
        this.updateLyricsDisplay();
    }

    // Nudges the lagging track back in line whenever both vocal and
    // backing are audible together — independent <audio> elements drift
    // apart over time even when started simultaneously.
    correctDrift() {
        const vocal = this.audioVocal;
        const backing = this.audioBacking;
        if (!vocal || !backing) return;
        if (vocal.paused || backing.paused) return; // one is muted/stopped

        const drift = vocal.currentTime - backing.currentTime;
        if (Math.abs(drift) > this.DRIFT_TOLERANCE) {
            backing.currentTime = vocal.currentTime;
        }
    }

    updateLyricsDisplay() {
        if (this.lyrics.length === 0) {
            this.callbacks.onLyricsUpdate({ currentLine: null });
            return;
        }

        let currentLineIdx = 0;
        let lineStartIdx = 0;

        for (let i = 0; i < this.lineIndices.length; i++) {
            if (i === this.lineIndices.length - 1 || this.currentLineIndex < this.lineIndices[i + 1]) {
                currentLineIdx = i;
                lineStartIdx = this.lineIndices[i];
                break;
            }
        }

        const nextLineStart = (currentLineIdx + 1 < this.lineIndices.length) ? 
            this.lineIndices[currentLineIdx + 1] : this.lyrics.length;
        const currentLineSyllables = this.lyrics.slice(lineStartIdx, nextLineStart);
        const wordInLine = this.currentLineIndex - lineStartIdx;

        const currentLine = currentLineSyllables.map((syl, idx) => ({
            text: syl,
            isHighlighted: idx === wordInLine
        }));

        const nextLine = (currentLineIdx + 1 < this.lyricsLines.length) ? 
            this.lyricsLines[currentLineIdx + 1] : null;

        this.callbacks.onLyricsUpdate({
            currentLine,
            nextLine,
            currentLineIndex: this.currentLineIndex,
            lineIndex: currentLineIdx,
            currentTime: this.getCurrentTime(),
            duration: this.getDuration()
        });
    }

    // Navigation
    nextLine() {
        this.currentLineIndex = Math.min(this.currentLineIndex + 1, this.lyrics.length - 1);
        if (this.syncTimes[this.currentLineIndex] !== undefined) {
            const seekTime = (this.syncTimes[this.currentLineIndex] - this.syncOffset) / 1000;
            this.seek(seekTime);
        }
        this.updateSync();
    }

    previousLine() {
        this.currentLineIndex = Math.max(this.currentLineIndex - 1, 0);
        if (this.syncTimes[this.currentLineIndex] !== undefined) {
            const seekTime = (this.syncTimes[this.currentLineIndex] - this.syncOffset) / 1000;
            this.seek(seekTime);
        }
        this.updateSync();
    }

    goToLine(index) {
        index = Math.max(0, Math.min(index, this.lyrics.length - 1));
        this.currentLineIndex = index;
        if (this.syncTimes[index] !== undefined) {
            const seekTime = (this.syncTimes[index] - this.syncOffset) / 1000;
            this.seek(seekTime);
        }
        this.updateSync();
    }

    // Utilities
    formatTime(sec) {
        if (!sec || isNaN(sec)) return '0:00';
        const m = Math.floor(sec / 60);
        const s = Math.floor(sec % 60);
        return `${m}:${s.toString().padStart(2, '0')}`;
    }

    getCurrentTime() {
        if (this.audioVocal && !this.audioVocal.paused) return this.audioVocal.currentTime;
        if (this.audioBacking && !this.audioBacking.paused) return this.audioBacking.currentTime;
        return 0;
    }

    getDuration() {
        if (this.audioVocal) return this.audioVocal.duration;
        if (this.audioBacking) return this.audioBacking.duration;
        return 0;
    }

    isPlaying() {
        return (this.audioVocal && !this.audioVocal.paused) || (this.audioBacking && !this.audioBacking.paused);
    }

    // State
    getState() {
        return {
            currentLineIndex: this.currentLineIndex,
            lyrics: this.lyrics,
            lyricsLines: this.lyricsLines,
            syncTimes: this.syncTimes,
            lineIndices: this.lineIndices,
            currentTime: this.getCurrentTime(),
            duration: this.getDuration(),
            isPlaying: this.isPlaying(),
            vocalMuted: this.vocalMuted,
            backingMuted: this.backingMuted,
            syncOffset: this.syncOffset
        };
    }

    reset() {
        this.currentLineIndex = 0;
        this.lyrics = [];
        this.syncTimes = [];
        this.syncOffset = 0;
        if (this.audioVocal) {
            this.audioVocal.pause();
            this.audioVocal = null;
        }
        if (this.audioBacking) {
            this.audioBacking.pause();
            this.audioBacking = null;
        }
        this.vocalMuted = false;
        this.backingMuted = false;
        this.autoAdvance = false;
        this.backingFilename = null;
        this.vocalFilenames = [];
    }
}

export default KaraokePlayer;
