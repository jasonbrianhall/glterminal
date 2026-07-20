        const vizWrap = document.getElementById('vizWrap');
        const vizCanvas = document.getElementById('vizCanvas');
        const vizCtx = vizCanvas.getContext('2d');
        const vizModeBtn = document.getElementById('vizModeBtn');
        const vizExportBtn = document.getElementById('vizExportBtn');
        const VIZ_MODES = ['Symmetry', 'Radial', 'Scope', 'Starfield', 'Plasma', 'Fireworks', 'Matrix', 'Kaleidoscope'];
        let vizMode = 0;
        let vizAnimHandle = null;
        let audioCtx = null, analyser = null, freqData = null, timeData = null;
        let vizStars = null, vizSparks = [], vizLastBurst = -1, vizMatrixCols = null;
        let exportDestNode = null;
        let isExporting = false;
        let activeRecorder = null;

        vizModeBtn.textContent = `Viz: ${VIZ_MODES[vizMode]}`;
        vizModeBtn.addEventListener('click', () => {
            vizMode = (vizMode + 1) % VIZ_MODES.length;
            vizModeBtn.textContent = `Viz: ${VIZ_MODES[vizMode]}`;
        });

        vizCanvas.addEventListener('dblclick', () => {
            // When borrowed as the karaoke lyrics background (see
            // attachVizAsKfnBackground in karaokePlayer.js), fullscreen is
            // handled by kfnLyricsWrap's own dblclick handler instead, so the
            // lyric text (which lives outside this canvas) comes along too.
            if (vizCanvas.closest('.kfn-viz-bg')) return;
            if (document.fullscreenElement) {
                document.exitFullscreen();
            } else {
                vizCanvas.requestFullscreen();
            }
        });

        function ensureAudioGraph() {
            if (audioCtx) {
                if (audioCtx.state === 'suspended') audioCtx.resume();
                return;
            }
            audioCtx = new (window.AudioContext || window.webkitAudioContext)();
            const source = audioCtx.createMediaElementSource(musicAudio);
            analyser = audioCtx.createAnalyser();
            analyser.fftSize = 512;
            analyser.smoothingTimeConstant = 0.8;
            source.connect(analyser);
            analyser.connect(audioCtx.destination);
            // Karaoke backing (instrumental) plays through its own <audio>
            // element (musicBackingAudio, see karaokePlayer.js). Route it into
            // the same analyser so the visualizer reacts to the full mix
            // instead of just the vocal track. Once routed through Web Audio,
            // its output only reaches speakers via this connection.
            const backingSource = audioCtx.createMediaElementSource(musicBackingAudio);
            backingSource.connect(analyser);
            freqData = new Uint8Array(analyser.frequencyBinCount);
            timeData = new Uint8Array(analyser.frequencyBinCount);
        }

        // Records the visualizer canvas + audio in real time (as long as the track itself)
        // and downloads the result as MP4 (falling back to WebM if the browser can't mux MP4).
        async function exportVisualizationVideo() {
            if (isExporting) return;
            if (!vizCanvas.captureStream || typeof MediaRecorder === 'undefined') {
                alert('Video export is not supported in this browser.');
                return;
            }
            if (!musicAudio.duration || !isFinite(musicAudio.duration)) {
                alert('Track is not ready yet.');
                return;
            }

            ensureAudioGraph();
            if (!exportDestNode) {
                exportDestNode = audioCtx.createMediaStreamDestination();
                analyser.connect(exportDestNode);
            }

            const mimeCandidates = [
                'video/mp4;codecs=avc1.42E01E,mp4a.40.2',
                'video/mp4',
                'video/webm;codecs=vp9,opus',
                'video/webm;codecs=vp8,opus',
                'video/webm'
            ];
            const mimeType = mimeCandidates.find(m => MediaRecorder.isTypeSupported(m));
            if (!mimeType) {
                alert('No supported recording format found in this browser.');
                return;
            }

            const canvasStream = vizCanvas.captureStream(30);
            const combinedStream = new MediaStream([
                ...canvasStream.getVideoTracks(),
                ...exportDestNode.stream.getAudioTracks()
            ]);

            const recordedChunks = [];
            const recorder = new MediaRecorder(combinedStream, { mimeType });
            activeRecorder = recorder;
            recorder.ondataavailable = (e) => { if (e.data.size > 0) recordedChunks.push(e.data); };

            const ext = mimeType.startsWith('video/mp4') ? 'mp4' : 'webm';
            const track = musicTracks.find(t => t.id === currentTrackId);
            const baseName = (track ? track.name : 'visualization').replace(/\.[^.]+$/, '');

            isExporting = true;
            vizExportBtn.disabled = true;
            vizModeBtn.disabled = true;

            recorder.onstop = () => {
                isExporting = false;
                activeRecorder = null;
                vizExportBtn.disabled = false;
                vizModeBtn.disabled = false;
                vizExportBtn.textContent = '\u2b07 Export';

                const blob = new Blob(recordedChunks, { type: mimeType });
                const url = URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = url;
                a.download = `${baseName}.${ext}`;
                document.body.appendChild(a);
                a.click();
                a.remove();
                setTimeout(() => URL.revokeObjectURL(url), 5000);
            };

            const progressTick = () => {
                if (recorder.state !== 'recording') return;
                vizExportBtn.textContent = `Exporting ${formatTime(musicAudio.currentTime)} / ${formatTime(musicAudio.duration)}`;
                requestAnimationFrame(progressTick);
            };

            const onEnded = () => {
                musicAudio.removeEventListener('ended', onEnded);
                if (recorder.state === 'recording') recorder.stop();
            };
            musicAudio.addEventListener('ended', onEnded);

            musicAudio.currentTime = 0;
            await musicAudio.play();
            recorder.start();
            progressTick();
        }

        vizExportBtn.addEventListener('click', exportVisualizationVideo);

        // Export CDG as WebP video during playback (works same as visualization export)
        async function exportCdgVideo() {
            if (isCdgExporting) return;
            if (!cdgCanvas.captureStream || typeof MediaRecorder === 'undefined') {
                alert('Video export is not supported in this browser.');
                return;
            }
            if (!musicAudio.duration || !isFinite(musicAudio.duration)) {
                alert('Track is not ready yet.');
                return;
            }

            ensureAudioGraph();
            if (!exportDestNode) {
                exportDestNode = audioCtx.createMediaStreamDestination();
                analyser.connect(exportDestNode);
            }

            const mimeCandidates = [
                'video/webp',
                'video/mp4;codecs=avc1.42E01E,mp4a.40.2',
                'video/mp4',
                'video/webm;codecs=vp9,opus',
                'video/webm'
            ];
            const mimeType = mimeCandidates.find(m => MediaRecorder.isTypeSupported(m));
            if (!mimeType) {
                alert('No supported recording format found in this browser.');
                return;
            }

            const canvasStream = cdgCanvas.captureStream(30);
            const combinedStream = new MediaStream([
                ...canvasStream.getVideoTracks(),
                ...exportDestNode.stream.getAudioTracks()
            ]);

            const recordedChunks = [];
            const recorder = new MediaRecorder(combinedStream, { mimeType });
            cdgExportRecorder = recorder;
            recorder.ondataavailable = (e) => { if (e.data.size > 0) recordedChunks.push(e.data); };

            // Determine file extension based on mime type
            let ext = 'webp';
            if (mimeType.startsWith('video/mp4')) ext = 'mp4';
            else if (mimeType.startsWith('video/webm')) ext = 'webm';

            const track = musicTracks.find(t => t.id === currentTrackId);
            const baseName = (track ? track.name : 'cdg-export').replace(/\.[^.]+$/, '');

            isCdgExporting = true;
            cdgExportBtn.disabled = true;

            recorder.onstop = () => {
                isCdgExporting = false;
                cdgExportRecorder = null;
                cdgExportBtn.disabled = false;
                cdgExportBtn.textContent = '\u2b07 Export';

                const blob = new Blob(recordedChunks, { type: mimeType });
                const url = URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = url;
                a.download = `${baseName}.${ext}`;
                document.body.appendChild(a);
                a.click();
                a.remove();
                setTimeout(() => URL.revokeObjectURL(url), 5000);
            };

            const progressTick = () => {
                if (recorder.state !== 'recording') return;
                cdgExportBtn.textContent = `Exporting ${formatTime(musicAudio.currentTime)} / ${formatTime(musicAudio.duration)}`;
                requestAnimationFrame(progressTick);
            };

            const onEnded = () => {
                musicAudio.removeEventListener('ended', onEnded);
                if (recorder.state === 'recording') recorder.stop();
            };
            musicAudio.addEventListener('ended', onEnded);

            musicAudio.currentTime = 0;
            await musicAudio.play();
            recorder.start();
            progressTick();
        }

        cdgExportBtn.addEventListener('click', exportCdgVideo);

        function vizHsl(hue, sat, light) {
            return `hsl(${((hue % 1) + 1) % 1 * 360}, ${sat}%, ${light}%)`;
        }

        function drawVizSymmetry(w, h) {
            const bars = 64, bw = w / bars, midY = h / 2;
            for (let i = 0; i < bars; i++) {
                const amp = freqData[i] / 255;
                const barH = amp * h * 0.48;
                vizCtx.fillStyle = vizHsl(i / bars, 90, 55);
                vizCtx.fillRect(i * bw + 1, midY - barH, bw - 2, barH);
                vizCtx.globalAlpha = 0.5;
                vizCtx.fillRect(i * bw + 1, midY, bw - 2, barH);
                vizCtx.globalAlpha = 1;
            }
        }

        function drawVizRadial(w, h, t) {
            const cx = w / 2, cy = h / 2;
            const maxR = Math.min(w, h) * 0.46, innerR = maxR * 0.18;
            const bars = 64, rot = t * 0.4;
            for (let i = 0; i < bars; i++) {
                const amp = freqData[i] / 255;
                const angle = rot + (i / bars) * Math.PI * 2;
                const outerR = innerR + amp * (maxR - innerR);
                vizCtx.strokeStyle = vizHsl(i / bars + t * 0.02, 100, 55);
                vizCtx.lineWidth = Math.max(2, w / bars * 0.5);
                vizCtx.beginPath();
                vizCtx.moveTo(cx + Math.cos(angle) * innerR, cy + Math.sin(angle) * innerR);
                vizCtx.lineTo(cx + Math.cos(angle) * outerR, cy + Math.sin(angle) * outerR);
                vizCtx.stroke();
            }
            const pulse = 0.4 + 0.6 * (freqData[0] / 255);
            vizCtx.fillStyle = vizHsl(0.12, 90, 60);
            vizCtx.beginPath();
            vizCtx.arc(cx, cy, innerR * pulse, 0, Math.PI * 2);
            vizCtx.fill();
        }

        function drawVizScope(w, h) {
            vizCtx.strokeStyle = 'rgba(255,255,255,0.85)';
            vizCtx.lineWidth = 2;
            vizCtx.beginPath();
            const slice = w / timeData.length;
            for (let i = 0; i < timeData.length; i++) {
                const v = timeData[i] / 128 - 1;
                const x = i * slice, y = h / 2 + v * h * 0.45;
                if (i === 0) vizCtx.moveTo(x, y); else vizCtx.lineTo(x, y);
            }
            vizCtx.stroke();
        }

        function drawVizStarfield(w, h) {
            if (!vizStars) {
                vizStars = [];
                for (let i = 0; i < 200; i++) {
                    vizStars.push({
                        x: Math.random(), y: Math.random(),
                        speed: 0.002 + Math.random() * 0.01,
                        size: 1 + Math.random() * 3, hue: Math.random()
                    });
                }
            }
            const bass = freqData[0] / 255;
            for (const s of vizStars) {
                const dx = s.x - 0.5, dy = s.y - 0.5;
                const dist = Math.sqrt(dx * dx + dy * dy) + 0.001;
                const speed = s.speed * (1 + bass * 3);
                s.x += dx / dist * speed;
                s.y += dy / dist * speed;
                if (s.x < 0 || s.x > 1 || s.y < 0 || s.y > 1) {
                    s.x = 0.5 + (Math.random() - 0.5) * 0.05;
                    s.y = 0.5 + (Math.random() - 0.5) * 0.05;
                    s.speed = 0.002 + Math.random() * 0.01;
                }
                const dist2 = Math.sqrt((s.x - 0.5) ** 2 + (s.y - 0.5) ** 2);
                const bright = Math.min(1, dist2 * 3);
                vizCtx.fillStyle = `hsla(${s.hue * 360}, 70%, 60%, ${bright})`;
                const sz = s.size * (0.5 + dist2 * 2);
                vizCtx.fillRect(s.x * w - sz / 2, s.y * h - sz / 2, sz, sz);
            }
        }

        function drawVizPlasma(w, h, t) {
            const cols = 48, rows = 27, cw = w / cols, ch = h / rows;
            const bass = freqData[2] / 255;
            for (let py = 0; py < rows; py++) {
                for (let px = 0; px < cols; px++) {
                    const nx = px / cols, ny = py / rows;
                    let v = Math.sin(nx * 10 + t * 1.3) + Math.sin(ny * 10 - t * 1.1)
                        + Math.sin((nx + ny) * 10 + t * 0.7)
                        + Math.sin(Math.sqrt((nx - 0.5) ** 2 + (ny - 0.5) ** 2) * 20 - t * 2);
                    v = (v / 4) * (0.6 + 0.4 * bass);
                    vizCtx.fillStyle = vizHsl(v * 0.5 + 0.5 + t * 0.03, 85, 55);
                    vizCtx.fillRect(px * cw, py * ch, cw + 1, ch + 1);
                }
            }
        }

        function drawVizFireworks(w, h, t) {
            const bass = freqData[1] / 255;
            if ((bass > 0.75 && t - vizLastBurst > 0.25) || t - vizLastBurst > 1.2) {
                vizLastBurst = t;
                const bx = Math.random() * w, by = h * (0.2 + Math.random() * 0.3);
                const hue0 = Math.random();
                const count = 40 + Math.floor(Math.random() * 30);
                for (let i = 0; i < count; i++) {
                    const ang = (i / count) * Math.PI * 2;
                    const spd = 40 + Math.random() * 90;
                    vizSparks.push({
                        x: bx, y: by, vx: Math.cos(ang) * spd, vy: Math.sin(ang) * spd,
                        life: 1, hue: hue0 + Math.random() * 0.08
                    });
                }
            }
            const dt = 1 / 60;
            for (const s of vizSparks) {
                if (s.life <= 0) continue;
                s.x += s.vx * dt;
                s.y += s.vy * dt;
                s.vy += 60 * dt;
                s.life -= dt * 0.6;
                if (s.y > h) s.life = 0;
                if (s.life > 0) {
                    vizCtx.fillStyle = `hsla(${(s.hue % 1) * 360}, 90%, 60%, ${Math.max(0, s.life)})`;
                    vizCtx.fillRect(s.x - 1.5, s.y - 1.5, 3, 3);
                }
            }
            vizSparks = vizSparks.filter(s => s.life > 0);
            if (vizSparks.length > 4000) vizSparks.splice(0, vizSparks.length - 4000);
        }

        function drawVizMatrix(w, h) {
            const cols = 28;
            if (!vizMatrixCols) {
                vizMatrixCols = [];
                for (let i = 0; i < cols; i++) {
                    vizMatrixCols.push({ y: -Math.random() * h, speed: 60 + Math.random() * 90 });
                }
            }
            const colW = w / cols, charH = 16, trail = 14;
            vizCtx.font = '14px monospace';
            vizCtx.textBaseline = 'top';
            for (let c = 0; c < cols; c++) {
                const col = vizMatrixCols[c];
                const bandVal = freqData[c % freqData.length] / 255;
                col.y += (col.speed * (0.6 + 0.8 * bandVal)) / 60;
                if (col.y - trail * charH > h) {
                    col.y = -Math.random() * h * 0.5;
                    col.speed = 60 + Math.random() * 90;
                }
                for (let k = 0; k < trail; k++) {
                    const cy = col.y - k * charH;
                    if (cy < 0 || cy > h) continue;
                    const glyph = String.fromCharCode(33 + Math.floor(Math.random() * 90));
                    const fade = 1 - k / trail;
                    const light = k === 0 ? 90 : 30 + fade * 40;
                    vizCtx.fillStyle = `hsl(120, 80%, ${light}%)`;
                    vizCtx.fillText(glyph, c * colW, cy);
                }
            }
        }

        function drawVizKaleidoscope(w, h, t) {
            const cx = w / 2, cy = h / 2;
            const maxR = Math.min(w, h) * 0.48;
            const segments = 8, segAngle = Math.PI * 2 / segments, rot = t * 0.25;
            for (let i = 0; i < 64; i += 2) {
                const amp = freqData[i] / 255;
                const baseAngle = (i / 64) * segAngle;
                const rr = amp * maxR;
                vizCtx.fillStyle = vizHsl(i / 64 + t * 0.06, 95, 55);
                for (let seg = 0; seg < segments; seg++) {
                    let ang = rot + seg * segAngle + baseAngle;
                    if (seg % 2 === 1) ang = rot + seg * segAngle + (segAngle - baseAngle);
                    vizCtx.beginPath();
                    vizCtx.arc(cx + Math.cos(ang) * rr, cy + Math.sin(ang) * rr, 3, 0, Math.PI * 2);
                    vizCtx.fill();
                }
            }
        }

        // Burns the current karaoke lyric line onto the visualizer canvas so it's
        // actually part of the pixels captureStream() sees. The DOM overlay
        // (kfnLyricsCurrent/kfnLyricsNext in karaokePlayer.js) looks right on
        // screen but lives outside the canvas, so exportVisualizationVideo()
        // never picks it up — this mirrors that same word/line logic but draws
        // with fillText instead of setting innerHTML.
        function wrapLyricWords(words, maxWidth) {
            const spaceWidth = vizCtx.measureText(' ').width;
            const rows = [];
            let row = [], rowWidth = 0;
            words.forEach((word, idx) => {
                const width = vizCtx.measureText(word).width;
                const addWidth = row.length === 0 ? width : width + spaceWidth;
                if (rowWidth + addWidth > maxWidth && row.length > 0) {
                    rows.push(row);
                    row = [{ word, idx }];
                    rowWidth = width;
                } else {
                    row.push({ word, idx });
                    rowWidth += addWidth;
                }
            });
            if (row.length) rows.push(row);
            return rows;
        }

        function drawLyricRow(row, activeIdx, y, spaceWidth, highlightColor) {
            const totalWidth = row.reduce((sum, item, i) =>
                sum + vizCtx.measureText(item.word).width + (i > 0 ? spaceWidth : 0), 0);
            let x = (vizCanvas.width - totalWidth) / 2;
            row.forEach(item => {
                const wordWidth = vizCtx.measureText(item.word).width;
                if (item.idx === activeIdx) {
                    const savedShadow = vizCtx.shadowBlur;
                    vizCtx.shadowBlur = 0;
                    vizCtx.fillStyle = highlightColor;
                    vizCtx.fillRect(x - 3, y - 18, wordWidth + 6, 24);
                    vizCtx.shadowBlur = savedShadow;
                }
                vizCtx.fillStyle = 'white';
                vizCtx.fillText(item.word, x, y);
                x += wordWidth + spaceWidth;
            });
        }

        function drawKfnLyricsOnCanvas(w, h) {
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
            const currentWords = lyrics.slice(lineStart, nextLineStart);
            const nextText = (lineIdx + 1 < lyricsLines.length) ? lyricsLines[lineIdx + 1].replace(/\//g, '') : '';

            const accentColor = getComputedStyle(document.documentElement).getPropertyValue('--accent').trim() || '#667eea';
            const maxWidth = w - 40;

            // Dark scrim so the text stays legible over bright visualizer modes
            vizCtx.save();
            vizCtx.fillStyle = 'rgba(0, 0, 0, 0.45)';
            vizCtx.fillRect(0, h * 0.62, w, h * 0.38);
            vizCtx.restore();

            vizCtx.textAlign = 'left';
            vizCtx.textBaseline = 'alphabetic';
            vizCtx.shadowColor = 'rgba(0,0,0,0.9)';
            vizCtx.shadowBlur = 6;

            vizCtx.font = 'bold 20px sans-serif';
            const spaceWidth = vizCtx.measureText(' ').width;
            const lineHeight = 26;
            const rows = wrapLyricWords(currentWords, maxWidth);
            let y = h * 0.78 - (rows.length - 1) * lineHeight;
            rows.forEach(row => {
                drawLyricRow(row, wordInLine, y, spaceWidth, accentColor);
                y += lineHeight;
            });

            if (nextText) {
                vizCtx.font = '14px sans-serif';
                vizCtx.fillStyle = 'rgba(255,255,255,0.7)';
                const nextWidth = vizCtx.measureText(nextText).width;
                vizCtx.fillText(nextText, (w - nextWidth) / 2, y);
            }

            vizCtx.shadowBlur = 0;
        }

        function vizLoop() {
            if (!analyser) return;
            analyser.getByteFrequencyData(freqData);
            analyser.getByteTimeDomainData(timeData);
            const w = vizCanvas.width, h = vizCanvas.height;
            const t = musicAudio.currentTime;
            vizCtx.fillStyle = 'black';
            vizCtx.fillRect(0, 0, w, h);
            switch (vizMode) {
                case 0: drawVizSymmetry(w, h); break;
                case 1: drawVizRadial(w, h, t); break;
                case 2: drawVizScope(w, h); break;
                case 3: drawVizStarfield(w, h); break;
                case 4: drawVizPlasma(w, h, t); break;
                case 5: drawVizFireworks(w, h, t); break;
                case 6: drawVizMatrix(w, h); break;
                case 7: drawVizKaleidoscope(w, h, t); break;
            }
            // Only draw lyrics on canvas during export; otherwise show DOM overlay
            if (kfnActive && isExporting) drawKfnLyricsOnCanvas(w, h);
            vizAnimHandle = requestAnimationFrame(vizLoop);
        }

        function showVizVisual() {
            musicArt.style.display = 'none';
            vizWrap.style.display = 'block';
            ensureAudioGraph();
            if (!vizAnimHandle) vizLoop();
        }

        function hideVizVisual() {
            vizWrap.style.display = 'none';
            if (vizAnimHandle) cancelAnimationFrame(vizAnimHandle);
            vizAnimHandle = null;
        }

        let musicTracks = [];
        let currentTrackId = null;
        let musicSearchTerm = '';
        let musicSortState = { col: 'name', dir: 1 };
        let shuffleOn = false;
        let repeatOn = false;
        let isSeeking = false;
        let currentObjectUrl = null;
        let currentBackingObjectUrl = null;
        let cdgState = null;
        let cdgAnimHandle = null;
        let loadToken = 0;

        function formatTime(sec) {
            if (!isFinite(sec)) return '0:00';
            const m = Math.floor(sec / 60);
            const s = Math.floor(sec % 60).toString().padStart(2, '0');
            return `${m}:${s}`;
        }

        function escapeHtml(str) {
            const div = document.createElement('div');
            div.textContent = str;
            return div.innerHTML;
        }

        function getDisplayedTracks() {
            let list = musicTracks;
            const term = musicSearchTerm.trim().toLowerCase();
            if (term) list = list.filter(t => t.name.toLowerCase().includes(term));

            const { col, dir } = musicSortState;
            list = [...list].sort((a, b) => {
                let av, bv;
                if (col === 'cdg') {
                    av = a.hasCdg === true ? 1 : 0;
                    bv = b.hasCdg === true ? 1 : 0;
                } else if (col === 'type') {
                    av = a.type; bv = b.type;
                } else {
                    av = a.name.toLowerCase(); bv = b.name.toLowerCase();
                }
                if (av < bv) return -1 * dir;
                if (av > bv) return 1 * dir;
                return 0;
            });
            return list;
        }

        function updateMusicSortHeaders() {
            musicTable.querySelectorAll('th[data-col]').forEach(th => {
                th.classList.remove('sort-asc', 'sort-desc');
                if (th.dataset.col === musicSortState.col) {
                    th.classList.add(musicSortState.dir === 1 ? 'sort-asc' : 'sort-desc');
                }
            });
        }

        function renderMusicTable() {
            const list = getDisplayedTracks();
            musicTableBody.innerHTML = list.map(track => {
                const cdgLabel = track.hasCdg === null ? '&hellip;' : (track.hasCdg ? '&#10003;' : '&#8212;');
                return `<tr data-id="${track.id}" class="${track.id === currentTrackId ? 'playing' : ''}">` +
                    `<td>${escapeHtml(track.name)}</td>` +
                    `<td>${track.type}</td>` +
                    `<td class="music-cdg-cell">${cdgLabel}</td>` +
                    `</tr>`;
            }).join('');
            updateMusicSortHeaders();
        }

        musicTableBody.addEventListener('click', (e) => {
            const row = e.target.closest('tr[data-id]');
            if (row) playTrackById(parseInt(row.dataset.id, 10));
        });

        musicTable.querySelectorAll('th[data-col]').forEach(th => {
            th.addEventListener('click', () => {
                const col = th.dataset.col;
                if (musicSortState.col === col) {
                    musicSortState.dir *= -1;
                } else {
                    musicSortState = { col, dir: 1 };
                }
                renderMusicTable();
            });
        });

        musicSearchInput.addEventListener('input', () => {
            musicSearchTerm = musicSearchInput.value;
            renderMusicTable();
        });

        async function playTrackById(id) {
            if (isExporting) return;
            const track = musicTracks.find(t => t.id === id);
            if (!track) return;
            currentTrackId = id;
            const token = ++loadToken;

            renderMusicTable();
            clearCdgState();
            clearKfnState();
            if (currentObjectUrl) {
                URL.revokeObjectURL(currentObjectUrl);
                currentObjectUrl = null;
            }
            if (currentBackingObjectUrl) {
                URL.revokeObjectURL(currentBackingObjectUrl);
                currentBackingObjectUrl = null;
            }
            musicAudio.pause();

            if (track.type === 'zip') {
                musicTitle.textContent = `Loading ${track.name}...`;
                try {
                    const { audioUrl, cdgBytes, displayName } = await loadKaraokeZip(track);
                    if (token !== loadToken) { URL.revokeObjectURL(audioUrl); return; }
                    currentObjectUrl = audioUrl;
                    musicAudio.src = audioUrl;
                    musicTitle.textContent = displayName;
                    if (cdgBytes) {
                        cdgState = cdgCreateState(cdgBytes);
                        showCdgVisual();
                        cdgLoop();
                    } else {
                        showVizVisual();
                    }
                    musicAudio.play();
                } catch (err) {
                    if (token !== loadToken) return;
                    musicTitle.textContent = `Failed to load ${track.name}`;
                    console.error(err);
                }
            } else if (track.type === 'midi') {
                musicTitle.textContent = `Rendering ${track.name}...`;
                try {
                    const res = await fetch(track.src, { headers: { 'X-Render-Midi': '1' } });
                    if (!res.ok) throw new Error(`HTTP ${res.status}`);
                    const blob = await res.blob();
                    const audioUrl = URL.createObjectURL(blob);
                    if (token !== loadToken) { URL.revokeObjectURL(audioUrl); return; }
                    currentObjectUrl = audioUrl;
                    musicAudio.src = audioUrl;
                    musicTitle.textContent = track.name;
                    showVizVisual();
                    musicAudio.play();
                } catch (err) {
                    if (token !== loadToken) return;
                    musicTitle.textContent = `Failed to render ${track.name}`;
                    console.error(err);
                }
            } else if (track.type === 'voc') {
                musicTitle.textContent = `Converting ${track.name}...`;
                try {
                    const res = await fetch(track.src, { headers: { 'X-Render-Voc': '1' } });
                    if (!res.ok) throw new Error(`HTTP ${res.status}`);
                    const blob = await res.blob();
                    const audioUrl = URL.createObjectURL(blob);
                    if (token !== loadToken) { URL.revokeObjectURL(audioUrl); return; }
                    currentObjectUrl = audioUrl;
                    musicAudio.src = audioUrl;
                    musicTitle.textContent = track.name;
                    showVizVisual();
                    musicAudio.play();
                } catch (err) {
                    if (token !== loadToken) return;
                    musicTitle.textContent = `Failed to convert ${track.name}`;
                    console.error(err);
                }
            } else if (track.type === 'au') {
                musicTitle.textContent = `Converting ${track.name}...`;
                try {
                    const res = await fetch(track.src, { headers: { 'X-Render-Au': '1' } });
                    if (!res.ok) throw new Error(`HTTP ${res.status}`);
                    const blob = await res.blob();
                    const audioUrl = URL.createObjectURL(blob);
                    if (token !== loadToken) { URL.revokeObjectURL(audioUrl); return; }
                    currentObjectUrl = audioUrl;
                    musicAudio.src = audioUrl;
                    musicTitle.textContent = track.name;
                    showVizVisual();
                    musicAudio.play();
                } catch (err) {
                    if (token !== loadToken) return;
                    musicTitle.textContent = `Failed to convert ${track.name}`;
                    console.error(err);
                }
            } else if (track.type === 'aiff') {
                musicTitle.textContent = `Converting ${track.name}...`;
                try {
                    const res = await fetch(track.src, { headers: { 'X-Render-Aiff': '1' } });
                    if (!res.ok) throw new Error(`HTTP ${res.status}`);
                    const blob = await res.blob();
                    const audioUrl = URL.createObjectURL(blob);
                    if (token !== loadToken) { URL.revokeObjectURL(audioUrl); return; }
                    currentObjectUrl = audioUrl;
                    musicAudio.src = audioUrl;
                    musicTitle.textContent = track.name;
                    showVizVisual();
                    musicAudio.play();
                } catch (err) {
                    if (token !== loadToken) return;
                    musicTitle.textContent = `Failed to convert ${track.name}`;
                    console.error(err);
                }
            } else if (track.type === 'convertible') {
                musicTitle.textContent = `Converting ${track.name}...`;
                try {
                    const res = await fetch(track.src, { headers: { 'X-Convert-Audio': '1' } });
                    if (!res.ok) throw new Error(`HTTP ${res.status}`);
                    const blob = await res.blob();
                    const audioUrl = URL.createObjectURL(blob);
                    if (token !== loadToken) { URL.revokeObjectURL(audioUrl); return; }
                    currentObjectUrl = audioUrl;
                    musicAudio.src = audioUrl;
                    musicTitle.textContent = track.name;
                    showVizVisual();
                    musicAudio.play();
                } catch (err) {
                    if (token !== loadToken) return;
                    musicTitle.textContent = `Failed to convert ${track.name}`;
                    console.error(err);
                }
            } else if (track.type === 'kfn') {
                musicTitle.textContent = `Loading ${track.name}...`;
                try {
                    const { audioUrl, backingAudioUrl, title, artist, lyrics, lyricsLines, syncTimes, lineIndices } = await loadKaraokeKfn(track);
                    if (token !== loadToken) {
                        URL.revokeObjectURL(audioUrl);
                        if (backingAudioUrl) URL.revokeObjectURL(backingAudioUrl);
                        return;
                    }
                    currentObjectUrl = audioUrl;
                    musicAudio.src = audioUrl;
                    musicTitle.textContent = title ? (artist ? `${title} - ${artist}` : title) : track.name;

                    kfnHasBacking = !!backingAudioUrl;
                    kfnMuteBackingBtn.style.display = kfnHasBacking ? '' : 'none';
                    if (backingAudioUrl) {
                        currentBackingObjectUrl = backingAudioUrl;
                        musicBackingAudio.src = backingAudioUrl;
                    }

                    kfnActive = true;
                    if (lyrics.length > 0) {
                        kfnLyricsData = { lyrics, lyricsLines, syncTimes, lineIndices };
                        showKfnVisual();
                    } else {
                        showVizVisual();
                    }
                    kfnLoop();
                    musicAudio.play();
                    if (kfnHasBacking) musicBackingAudio.play();
                } catch (err) {
                    if (token !== loadToken) return;
                    musicTitle.textContent = `Failed to load ${track.name}`;
                    console.error(err);
                }
            } else {
                musicAudio.src = track.src;
                musicTitle.textContent = track.name;
                musicAudio.play();
                if (track.cdgSrc) {
                    try {
                        const resp = await fetch(track.cdgSrc);
                        const cdgBytes = new Uint8Array(await resp.arrayBuffer());
                        if (token !== loadToken) return;
                        cdgState = cdgCreateState(cdgBytes);
                        showCdgVisual();
                        cdgLoop();
                    } catch (err) {
                        console.error('Failed to load CD+G file', err);
                        showVizVisual();
                    }
                } else {
                    showVizVisual();
                }
            }
        }

        function nextTrack() {
            const list = getDisplayedTracks();
            if (list.length === 0) return;
            if (shuffleOn) {
                let idx = Math.floor(Math.random() * list.length);
                if (list.length > 1 && list[idx].id === currentTrackId) idx = (idx + 1) % list.length;
                playTrackById(list[idx].id);
            } else {
                const curIdx = list.findIndex(t => t.id === currentTrackId);
                playTrackById(list[(curIdx + 1 + list.length) % list.length].id);
            }
        }

        function prevTrack() {
            const list = getDisplayedTracks();
            if (list.length === 0) return;
            // If more than 3s into the track, restart it instead of going back
            const elapsed = musicAudio.currentTime;
            if (elapsed > 3) {
                musicAudio.currentTime = 0;
                return;
            }
            const curIdx = list.findIndex(t => t.id === currentTrackId);
            playTrackById(list[(curIdx - 1 + list.length) % list.length].id);
        }

        function openMusicPlayer() {
            musicTracks = getAudioEntries();
            if (musicTracks.length === 0) return;
            musicSearchTerm = '';
            musicSearchInput.value = '';
            musicSortState = { col: 'name', dir: 1 };
            musicOverlay.classList.add('active');

            // Kick off async CD+G detection for zip tracks without blocking playback
            musicTracks.filter(t => t.type === 'zip').forEach(t => {
                peekZipHasCdg(t.src).then(has => {
                    t.hasCdg = has;
                    renderMusicTable();
                });
            });

            const list = getDisplayedTracks();
            renderMusicTable();
            if (list.length > 0) playTrackById(list[0].id);
        }

        function closeMusicPlayer() {
            if (activeRecorder && activeRecorder.state === 'recording') activeRecorder.stop();
            musicOverlay.classList.remove('active');
            musicAudio.pause();
            musicAudio.src = '';
            currentTrackId = null;
            clearCdgState();
            clearKfnState();
            hideVizVisual();
            if (currentObjectUrl) {
                URL.revokeObjectURL(currentObjectUrl);
                currentObjectUrl = null;
            }
            if (currentBackingObjectUrl) {
                URL.revokeObjectURL(currentBackingObjectUrl);
                currentBackingObjectUrl = null;
            }
        }

        musicBtn.addEventListener('click', openMusicPlayer);
        document.getElementById('musicClose').addEventListener('click', closeMusicPlayer);

        musicOverlay.addEventListener('click', (e) => {
            if (e.target === musicOverlay) closeMusicPlayer();
        });

        musicPlayPause.addEventListener('click', () => {
            if (musicAudio.paused) {
                musicAudio.play();
            } else {
                musicAudio.pause();
            }
        });

        musicAudio.addEventListener('play', () => {
            musicPlayPause.innerHTML = '&#10074;&#10074;';
        });

        musicAudio.addEventListener('pause', () => {
            musicPlayPause.innerHTML = '&#9658;';
        });

        musicAudio.addEventListener('ended', () => {
            if (isExporting) return;
            if (repeatOn) {
                musicAudio.currentTime = 0;
                musicAudio.play();
            } else {
                nextTrack();
            }
        });

        musicAudio.addEventListener('timeupdate', () => {
            if (isSeeking) return;
            musicTimeCurrent.textContent = formatTime(musicAudio.currentTime);
            musicTimeDuration.textContent = formatTime(musicAudio.duration);
            if (musicAudio.duration) {
                musicSeek.value = (musicAudio.currentTime / musicAudio.duration) * 100;
            }
        });

        musicAudio.addEventListener('loadedmetadata', () => {
            musicTimeDuration.textContent = formatTime(musicAudio.duration);
        });

        musicSeek.addEventListener('input', () => {
            isSeeking = true;
            musicTimeCurrent.textContent = formatTime((musicSeek.value / 100) * musicAudio.duration);
        });

        musicSeek.addEventListener('change', () => {
            if (musicAudio.duration) {
                musicAudio.currentTime = (musicSeek.value / 100) * musicAudio.duration;
            }
            isSeeking = false;
        });

        document.getElementById('musicNext').addEventListener('click', nextTrack);
        document.getElementById('musicPrev').addEventListener('click', prevTrack);
        document.getElementById('musicUpBtn').addEventListener('click', prevTrack);
        document.getElementById('musicDownBtn').addEventListener('click', nextTrack);

        musicShuffleBtn.addEventListener('click', () => {
            shuffleOn = !shuffleOn;
            musicShuffleBtn.classList.toggle('active', shuffleOn);
        });

        musicRepeatBtn.addEventListener('click', () => {
            repeatOn = !repeatOn;
            musicRepeatBtn.classList.toggle('active', repeatOn);
        });

        document.addEventListener('keydown', (e) => {
            if (!musicOverlay.classList.contains('active')) return;
            if (e.key === 'Escape') { closeMusicPlayer(); return; }
            if (document.activeElement === musicSearchInput) return;
            if (e.key === 'ArrowRight') {
                e.preventDefault();
                musicAudio.currentTime = Math.min(musicAudio.duration || Infinity, musicAudio.currentTime + 10);
            } else if (e.key === 'ArrowLeft') {
                e.preventDefault();
                musicAudio.currentTime = Math.max(0, musicAudio.currentTime - 10);
            } else if (e.key === 'ArrowUp') {
                e.preventDefault();
                prevTrack();
            } else if (e.key === 'ArrowDown') {
                e.preventDefault();
                nextTrack();
            } else if (e.key === ' ') {
                e.preventDefault();
                musicPlayPause.click();
            } else if (e.key === 'v' || e.key === 'V') {
                vizModeBtn.click();
            }
        });

        // ---- video player ----