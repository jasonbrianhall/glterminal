        // KFN karaoke parser/renderer, backed by the bundled KaraokePlayer library.
        // karaoke-library.js is an ES module, so it's lazy-loaded via dynamic
        // import() the first time a .kfn track is played (mirrors how CD+G zip
        // packs are only unzipped on demand in cdgPlayer.js).
        let kfnLyricsData = null;
        let kfnAnimHandle = null;
        let kfnPlayerClass = null;
        let kfnActive = false;
        let kfnHasBacking = false;
        // musicBackingAudio never stays perfectly locked to musicAudio's own
        // clock, so drift beyond this (seconds) triggers a resync.
        const KFN_DRIFT_TOLERANCE = 0.08;

        async function getKaraokePlayerClass() {
            if (!kfnPlayerClass) {
                const mod = await import('/public/karaoke-library.js');
                kfnPlayerClass = mod.default;
            }
            return kfnPlayerClass;
        }

        // Parses a .kfn file (audio + synced lyrics) and returns both audio
        // tracks (vocal + backing, when present) plus lyric data, mirroring
        // loadKaraokeZip() for CD+G packs.
        async function loadKaraokeKfn(track) {
            const KaraokePlayer = await getKaraokePlayerClass();

            let meta = { title: null, artist: null };
            const player = new KaraokePlayer({
                onMetadataLoaded: (data) => { meta = data; }
            });

            const resp = await fetch(track.src);
            const buf = await resp.arrayBuffer();
            await player.parseKFN(buf);

            // musicAudio (vocal) stays the primary track — it drives the
            // visualizer/progress bar/transport controls as before.
            // musicBackingAudio plays the instrumental alongside it.
            const vocalEl = player.audioVocal || player.audioBacking;
            const backingEl = (player.audioVocal && player.audioBacking) ? player.audioBacking : null;
            if (!vocalEl) throw new Error('No audio track found in KFN file');

            return {
                audioUrl: vocalEl.src,
                backingAudioUrl: backingEl ? backingEl.src : null,
                title: meta.title,
                artist: meta.artist,
                lyrics: player.lyrics,
                lyricsLines: player.lyricsLines,
                syncTimes: player.syncTimes,
                lineIndices: player.lineIndices
            };
        }

        function kfnFindWordIndex(syncTimes, currentMs) {
            let idx = 0;
            for (let i = 0; i < syncTimes.length; i++) {
                if (currentMs >= syncTimes[i]) idx = i; else break;
            }
            return idx;
        }

        function kfnRenderFrame() {
            if (!kfnLyricsData || kfnLyricsData.lyrics.length === 0) return;
            const { lyrics, lyricsLines, syncTimes, lineIndices } = kfnLyricsData;
            const wordIdx = kfnFindWordIndex(syncTimes, musicAudio.currentTime * 1000);

            let lineIdx = 0, lineStart = 0;
            for (let i = 0; i < lineIndices.length; i++) {
                if (i === lineIndices.length - 1 || wordIdx < lineIndices[i + 1]) {
                    lineIdx = i;
                    lineStart = lineIndices[i];
                    break;
                }
            }
            const nextLineStart = (lineIdx + 1 < lineIndices.length) ? lineIndices[lineIdx + 1] : lyrics.length;
            const wordInLine = wordIdx - lineStart;

            kfnLyricsCurrent.innerHTML = lyrics.slice(lineStart, nextLineStart).map((word, i) =>
                `<span class="kfn-syllable${i === wordInLine ? ' kfn-highlighted' : ''}">${escapeHtml(word)}</span>`
            ).join(' ');
            kfnLyricsNext.textContent = (lineIdx + 1 < lyricsLines.length) ? lyricsLines[lineIdx + 1].replace(/\//g, '') : '';
        }

        // Keeps the backing track locked to the vocal track's position.
        // Two independent <audio> elements drift apart over time even when
        // started together, and this also re-syncs after any seek (arrow
        // keys, seek bar, repeat) without needing to special-case each one.
        function correctKfnBackingDrift() {
            if (!kfnHasBacking) return;
            if (musicAudio.paused || musicBackingAudio.paused) return;
            const drift = musicAudio.currentTime - musicBackingAudio.currentTime;
            if (Math.abs(drift) > KFN_DRIFT_TOLERANCE) {
                musicBackingAudio.currentTime = musicAudio.currentTime;
            }
        }

        function kfnLoop() {
            if (!kfnActive) return;
            correctKfnBackingDrift();
            kfnRenderFrame();
            kfnAnimHandle = requestAnimationFrame(kfnLoop);
        }

        // Hides the vocal/backing mute buttons after a period of mouse
        // inactivity, mirroring typical video-player control behavior.
        const KFN_CONTROLS_IDLE_MS = 2000;
        let kfnControlsIdleTimer = null;

        function showKfnTrackControls() {
            kfnTrackControls.classList.remove('controls-hidden');
            if (kfnControlsIdleTimer) clearTimeout(kfnControlsIdleTimer);
            kfnControlsIdleTimer = setTimeout(() => {
                kfnTrackControls.classList.add('controls-hidden');
            }, KFN_CONTROLS_IDLE_MS);
        }

        function stopKfnControlsIdleWatch() {
            if (kfnControlsIdleTimer) clearTimeout(kfnControlsIdleTimer);
            kfnControlsIdleTimer = null;
            kfnTrackControls.classList.remove('controls-hidden');
        }

        function showKfnVisual() {
            musicArt.style.display = 'none';
            hideVizVisual();
            kfnLyricsWrap.style.display = 'flex';
            showKfnTrackControls();
        }

        function hideKfnVisual() {
            kfnLyricsWrap.style.display = 'none';
            musicArt.style.display = 'flex';
            stopKfnControlsIdleWatch();
        }

        function clearKfnState() {
            if (kfnAnimHandle) cancelAnimationFrame(kfnAnimHandle);
            kfnAnimHandle = null;
            kfnLyricsData = null;
            kfnActive = false;
            kfnHasBacking = false;
            musicBackingAudio.pause();
            musicBackingAudio.removeAttribute('src');
            musicBackingAudio.load();
            musicAudio.muted = false;
            musicBackingAudio.muted = false;
            kfnMuteVocalBtn.classList.remove('muted');
            kfnMuteBackingBtn.classList.remove('muted');
            kfnMuteVocalBtn.innerHTML = '&#127908; Vocal';
            kfnMuteBackingBtn.innerHTML = '&#127925; Backing';
            kfnMuteBackingBtn.style.display = '';
            hideKfnVisual();
        }

        const kfnLyricsWrap = document.getElementById('kfnLyricsWrap');
        const kfnLyricsCurrent = document.getElementById('kfnLyricsCurrent');
        const kfnLyricsNext = document.getElementById('kfnLyricsNext');
        const musicBackingAudio = document.getElementById('musicBackingAudio');
        const kfnMuteVocalBtn = document.getElementById('kfnMuteVocalBtn');
        const kfnMuteBackingBtn = document.getElementById('kfnMuteBackingBtn');
        const kfnTrackControls = document.getElementById('kfnTrackControls');

        kfnMuteVocalBtn.addEventListener('click', () => {
            musicAudio.muted = !musicAudio.muted;
            kfnMuteVocalBtn.classList.toggle('muted', musicAudio.muted);
            kfnMuteVocalBtn.innerHTML = musicAudio.muted ? '&#127908; Unmute Vocal' : '&#127908; Vocal';
        });

        kfnMuteBackingBtn.addEventListener('click', () => {
            if (!kfnHasBacking) return;
            musicBackingAudio.muted = !musicBackingAudio.muted;
            kfnMuteBackingBtn.classList.toggle('muted', musicBackingAudio.muted);
            kfnMuteBackingBtn.innerHTML = musicBackingAudio.muted ? '&#127925; Unmute Backing' : '&#127925; Backing';
        });

        kfnLyricsWrap.addEventListener('dblclick', (e) => {
            if (e.target.closest('.kfn-track-controls')) return;
            if (document.fullscreenElement) {
                document.exitFullscreen();
            } else {
                kfnLyricsWrap.requestFullscreen();
            }
        });

        kfnLyricsWrap.addEventListener('mousemove', showKfnTrackControls);

        // musicAudio (vocal) is the transport's source of truth — every
        // play/pause path (button, spacebar, repeat, playTrackById) ends up
        // calling musicAudio.play()/.pause(), so mirroring those events onto
        // musicBackingAudio covers all of them without touching each call site.
        musicAudio.addEventListener('play', () => {
            if (kfnHasBacking && musicBackingAudio.paused) musicBackingAudio.play();
        });

        musicAudio.addEventListener('pause', () => {
            if (kfnHasBacking && !musicBackingAudio.paused) musicBackingAudio.pause();
        });
