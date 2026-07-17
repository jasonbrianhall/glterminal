        // Native browser video formats
        const VIDEO_EXTENSIONS = ['mp4', 'webm', 'ogv', 'ogg', 'mov', 'm4v', 'mkv', 'avi', 'flv', 'wmv'];
        const SUBTITLE_EXTENSIONS = ['vtt', 'srt'];

        function isVideoFile(entry) {
            if (entry.type !== 'file') return false;
            const dot = entry.name.lastIndexOf('.');
            if (dot === -1) return false;
            const ext = entry.name.slice(dot + 1).toLowerCase();
            return VIDEO_EXTENSIONS.includes(ext);
        }

        function getVideoEntries() {
            // Look for standalone "movie.mp4" + "movie.vtt"/"movie.srt" pairs
            const subsByBaseName = new Map();
            fileData.forEach(entry => {
                if (entry.type !== 'file') return;
                const dot = entry.name.lastIndexOf('.');
                if (dot === -1) return;
                const ext = entry.name.slice(dot + 1).toLowerCase();
                if (SUBTITLE_EXTENSIONS.includes(ext)) {
                    subsByBaseName.set(entry.name.slice(0, dot).toLowerCase(), entry.name);
                }
            });

            const videos = fileData.filter(isVideoFile).map(e => {
                const track = entryToTrack(e, 'video');
                const dot = e.name.lastIndexOf('.');
                const base = (dot === -1 ? e.name : e.name.slice(0, dot)).toLowerCase();
                if (subsByBaseName.has(base)) {
                    track.hasSubs = true;
                    track.subSrc = entryToTrack({ name: subsByBaseName.get(base) }, 'subtitle').src;
                    track.subIsSrt = subsByBaseName.get(base).toLowerCase().endsWith('.srt');
                } else {
                    track.hasSubs = false;
                }
                return track;
            });

            return videos.map((t, i) => ({ ...t, id: i }));
        }

        const slideshowBtn = document.getElementById('slideshowBtn');
        const slideshowOverlay = document.getElementById('slideshowOverlay');
        const slideshowImg = document.getElementById('slideshowImg');
        const slideshowCaption = document.getElementById('slideshowCaption');
        let slideshowImages = [];
        let slideshowIndex = 0;

        function updateSlideshowButtonVisibility() {
            const images = getImageEntries();
            slideshowBtn.style.display = images.length > 0 ? 'inline-block' : 'none';
            const audios = getAudioEntries();
            musicBtn.style.display = audios.length > 0 ? 'inline-block' : 'none';
            const videos = getVideoEntries();
            videoBtn.style.display = videos.length > 0 ? 'inline-block' : 'none';
        }

        // ---- zoom/pan state ----