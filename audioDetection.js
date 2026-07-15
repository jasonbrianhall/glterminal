        // Native browser formats (no conversion needed)
        const AUDIO_EXTENSIONS = [
          'mp3',   // OS codec
          'aac',   // OS codec
          'm4a',   // AAC only
          'mp4',   // AAC only
          'wav',   // built-in
          'ogg',   // built-in (Vorbis/Opus)
          'flac',  // built-in
          'opus',  // built-in
          'webm',   // built-in (Opus/Vorbis)
          'mkv',    // Matroska Movies (but just the audio part)
        ];

        // Formats that need conversion to WAV
        const MIDI_EXTENSIONS = ['mid', 'midi', 'kar'];
        const VOC_EXTENSIONS = ['voc'];
        const AU_EXTENSIONS = ['au', 'snd'];
        const AIFF_EXTENSIONS = ['aiff', 'aif'];
        const CONVERTIBLE_AUDIO_EXTENSIONS = ['m4a', 'wma', 'mp2', 'mpa'];  // Converted to WAV on server

        function isAudioFile(entry) {
            if (entry.type !== 'file') return false;
            const dot = entry.name.lastIndexOf('.');
            if (dot === -1) return false;
            const ext = entry.name.slice(dot + 1).toLowerCase();
            return AUDIO_EXTENSIONS.includes(ext);
        }

        function isMidiFile(entry) {
            if (entry.type !== 'file') return false;
            const dot = entry.name.lastIndexOf('.');
            if (dot === -1) return false;
            const ext = entry.name.slice(dot + 1).toLowerCase();
            return MIDI_EXTENSIONS.includes(ext);
        }

        function isVocFile(entry) {
            if (entry.type !== 'file') return false;
            const dot = entry.name.lastIndexOf('.');
            if (dot === -1) return false;
            const ext = entry.name.slice(dot + 1).toLowerCase();
            return VOC_EXTENSIONS.includes(ext);
        }

        function isAuFile(entry) {
            if (entry.type !== 'file') return false;
            const dot = entry.name.lastIndexOf('.');
            if (dot === -1) return false;
            const ext = entry.name.slice(dot + 1).toLowerCase();
            return AU_EXTENSIONS.includes(ext);
        }

        function isAiffFile(entry) {
            if (entry.type !== 'file') return false;
            const dot = entry.name.lastIndexOf('.');
            if (dot === -1) return false;
            const ext = entry.name.slice(dot + 1).toLowerCase();
            return AIFF_EXTENSIONS.includes(ext);
        }

        function isConvertibleAudioFile(entry) {
            if (entry.type !== 'file') return false;
            const dot = entry.name.lastIndexOf('.');
            if (dot === -1) return false;
            const ext = entry.name.slice(dot + 1).toLowerCase();
            return CONVERTIBLE_AUDIO_EXTENSIONS.includes(ext);
        }

        function isZipFile(entry) {
            if (entry.type !== 'file') return false;
            return entry.name.toLowerCase().endsWith('.zip');
        }

        function entryToTrack(entry, type) {
            const currentPath = getCurrentPath();
            const entryPath = currentPath.endsWith('/')
                ? `${currentPath}${entry.name}`
                : `${currentPath}/${entry.name}`;
            return { name: entry.name, src: encodeParens(entryPath), type };
        }

        function getAudioEntries() {
            // Look for standalone "song.mp3" + "song.cdg" pairs sitting next to each other
            const cdgByBaseName = new Map();
            fileData.forEach(entry => {
                if (entry.type === 'file' && entry.name.toLowerCase().endsWith('.cdg')) {
                    const base = entry.name.slice(0, -4).toLowerCase();
                    cdgByBaseName.set(base, entry.name);
                }
            });

            const audio = fileData.filter(isAudioFile).map(e => {
                const track = entryToTrack(e, 'audio');
                const dot = e.name.lastIndexOf('.');
                const base = (dot === -1 ? e.name : e.name.slice(0, dot)).toLowerCase();
                if (cdgByBaseName.has(base)) {
                    track.hasCdg = true;
                    track.cdgSrc = entryToTrack({ name: cdgByBaseName.get(base) }, 'cdg').src;
                } else {
                    track.hasCdg = false;
                }
                return track;
            });

            // Zip files are checked lazily (peeked async) since we'd need to fetch them to know
            const zips = fileData.filter(isZipFile).map(e => {
                const track = entryToTrack(e, 'zip');
                track.hasCdg = null; // unknown until peeked
                return track;
            });

            const midis = fileData.filter(isMidiFile).map(e => entryToTrack(e, 'midi'));

            const vocs = fileData.filter(isVocFile).map(e => entryToTrack(e, 'voc'));

            const aus = fileData.filter(isAuFile).map(e => entryToTrack(e, 'au'));

            const aiffs = fileData.filter(isAiffFile).map(e => entryToTrack(e, 'aiff'));

            const convertibles = fileData.filter(isConvertibleAudioFile).map(e => entryToTrack(e, 'convertible'));

            return [...audio, ...zips, ...midis, ...vocs, ...aus, ...aiffs, ...convertibles].map((t, i) => ({ ...t, id: i }));
        }

        // ---- video detection ----