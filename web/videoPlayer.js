        const videoBtn = document.getElementById('videoBtn');
        const videoOverlay = document.getElementById('videoOverlay');
        const videoElement = document.getElementById('videoElement');
        const videoTitle = document.getElementById('videoTitle');
        const videoSearchInput = document.getElementById('videoSearch');
        const videoTable = document.getElementById('videoTable');
        const videoTableBody = document.getElementById('videoTableBody');

        let videoTracks = [];
        let currentVideoTrackId = null;
        let videoSearchTerm = '';
        let videoSortState = { col: 'name', dir: 1 };
        let currentSubtitleUrl = null;

        // Converts SRT timestamps/format to WebVTT so the browser's native <track> can use it
        function srtToVtt(srtText) {
            const body = srtText
                .replace(/\r/g, '')
                .replace(/(\d{2}:\d{2}:\d{2}),(\d{3})/g, '$1.$2');
            return 'WEBVTT\n\n' + body;
        }

        function getDisplayedVideos() {
            let list = videoTracks;
            const term = videoSearchTerm.trim().toLowerCase();
            if (term) list = list.filter(t => t.name.toLowerCase().includes(term));

            const { col, dir } = videoSortState;
            list = [...list].sort((a, b) => {
                let av, bv;
                if (col === 'subs') {
                    av = a.hasSubs ? 1 : 0;
                    bv = b.hasSubs ? 1 : 0;
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

        function updateVideoSortHeaders() {
            videoTable.querySelectorAll('th[data-col]').forEach(th => {
                th.classList.remove('sort-asc', 'sort-desc');
                if (th.dataset.col === videoSortState.col) {
                    th.classList.add(videoSortState.dir === 1 ? 'sort-asc' : 'sort-desc');
                }
            });
        }

        function renderVideoTable() {
            const list = getDisplayedVideos();
            videoTableBody.innerHTML = list.map(track => {
                const subsLabel = track.hasSubs ? '&#10003;' : '&#8212;';
                return `<tr data-id="${track.id}" class="${track.id === currentVideoTrackId ? 'playing' : ''}">` +
                    `<td>${escapeHtml(track.name)}</td>` +
                    `<td>${track.type}</td>` +
                    `<td class="music-cdg-cell">${subsLabel}</td>` +
                    `</tr>`;
            }).join('');
            updateVideoSortHeaders();
        }

        videoTableBody.addEventListener('click', (e) => {
            const row = e.target.closest('tr[data-id]');
            if (row) playVideoById(parseInt(row.dataset.id, 10));
        });

        videoTable.querySelectorAll('th[data-col]').forEach(th => {
            th.addEventListener('click', () => {
                const col = th.dataset.col;
                if (videoSortState.col === col) {
                    videoSortState.dir *= -1;
                } else {
                    videoSortState = { col, dir: 1 };
                }
                renderVideoTable();
            });
        });

        videoSearchInput.addEventListener('input', () => {
            videoSearchTerm = videoSearchInput.value;
            renderVideoTable();
        });

        async function playVideoById(id) {
            const track = videoTracks.find(t => t.id === id);
            if (!track) return;
            currentVideoTrackId = id;
            renderVideoTable();

            // Clear any existing subtitle track
            videoElement.querySelectorAll('track').forEach(t => t.remove());
            if (currentSubtitleUrl) {
                URL.revokeObjectURL(currentSubtitleUrl);
                currentSubtitleUrl = null;
            }

            videoElement.src = track.src;
            videoTitle.textContent = track.name;
            videoElement.play();

            if (track.subSrc) {
                try {
                    const resp = await fetch(track.subSrc);
                    let vttText = await resp.text();
                    if (track.subIsSrt) vttText = srtToVtt(vttText);
                    const blob = new Blob([vttText], { type: 'text/vtt' });
                    currentSubtitleUrl = URL.createObjectURL(blob);
                    const trackEl = document.createElement('track');
                    trackEl.kind = 'subtitles';
                    trackEl.label = 'Subtitles';
                    trackEl.srclang = 'en';
                    trackEl.src = currentSubtitleUrl;
                    trackEl.default = true;
                    videoElement.appendChild(trackEl);
                } catch (err) {
                    console.error('Failed to load subtitles', err);
                }
            }
        }

        function nextVideo() {
            const list = getDisplayedVideos();
            if (list.length === 0) return;
            const curIdx = list.findIndex(t => t.id === currentVideoTrackId);
            playVideoById(list[(curIdx + 1 + list.length) % list.length].id);
        }

        function prevVideo() {
            const list = getDisplayedVideos();
            if (list.length === 0) return;
            const curIdx = list.findIndex(t => t.id === currentVideoTrackId);
            playVideoById(list[(curIdx - 1 + list.length) % list.length].id);
        }

        function isMusicPlayableFile(entry) {
            return isAudioFile(entry) || isMidiFile(entry) || isVocFile(entry) ||
                   isAuFile(entry) || isAiffFile(entry) || isConvertibleAudioFile(entry) || isKfnFile(entry);
            // .zip is deliberately excluded here — not every zip is a karaoke pack,
            // so a bare click shouldn't assume that. The Music library button still
            // lists and lazily peeks zips for CD+G content.
        }

        function openMusicFromTable(filePath, name) {
            // If open-in-new-window is enabled, just open the file normally
            if (openInNewWindow) {
                window.open(filePath, '_blank');
                return;
            }

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

            renderMusicTable();

            const track = musicTracks.find(t => t.name === name);
            playTrackById(track ? track.id : musicTracks[0].id);
        }

        function openVideoFromTable(filePath, name) {
            // If open-in-new-window is enabled, just open the file normally
            if (openInNewWindow) {
                window.open(filePath, '_blank');
                return;
            }

            videoTracks = getVideoEntries();
            if (videoTracks.length === 0) return;

            videoSearchTerm = '';
            videoSearchInput.value = '';
            videoSortState = { col: 'name', dir: 1 };
            videoOverlay.classList.add('active');
            renderVideoTable();

            const track = videoTracks.find(t => t.name === name);
            playVideoById(track ? track.id : videoTracks[0].id);
        }

        function openVideoPlayer() {
            videoTracks = getVideoEntries();
            if (videoTracks.length === 0) return;
            videoSearchTerm = '';
            videoSearchInput.value = '';
            videoSortState = { col: 'name', dir: 1 };
            videoOverlay.classList.add('active');
            renderVideoTable();
            const list = getDisplayedVideos();
            if (list.length > 0) playVideoById(list[0].id);
        }

        function closeVideoPlayer() {
            videoOverlay.classList.remove('active');
            videoElement.pause();
            videoElement.removeAttribute('src');
            videoElement.load();
            videoElement.querySelectorAll('track').forEach(t => t.remove());
            currentVideoTrackId = null;
            if (currentSubtitleUrl) {
                URL.revokeObjectURL(currentSubtitleUrl);
                currentSubtitleUrl = null;
            }
        }

        videoBtn.addEventListener('click', openVideoPlayer);
        document.getElementById('videoClose').addEventListener('click', closeVideoPlayer);
        document.getElementById('videoUpBtn').addEventListener('click', prevVideo);
        document.getElementById('videoDownBtn').addEventListener('click', nextVideo);

        videoOverlay.addEventListener('click', (e) => {
            if (e.target === videoOverlay) closeVideoPlayer();
        });

        document.addEventListener('keydown', (e) => {
            if (!videoOverlay.classList.contains('active')) return;
            if (e.key === 'Escape') { closeVideoPlayer(); return; }
            if (document.activeElement === videoSearchInput) return;
            if (e.key === 'ArrowUp') {
                e.preventDefault();
                prevVideo();
            } else if (e.key === 'ArrowDown') {
                e.preventDefault();
                nextVideo();
            }
        });

        // Cookie management for navigation history
        function setCookie(name, value, days = 30) {
            const date = new Date();
            date.setTime(date.getTime() + (days * 24 * 60 * 60 * 1000));
            const expires = `expires=${date.toUTCString()}`;
            document.cookie = `${name}=${encodeURIComponent(value)};${expires};path=/`;
        }

        function getCookie(name) {
            const nameEQ = `${name}=`;
            const cookies = document.cookie.split(';');
            for (let i = 0; i < cookies.length; i++) {
                let c = cookies[i].trim();
                if (c.indexOf(nameEQ) === 0) return decodeURIComponent(c.substring(nameEQ.length));
            }
            return null;
        }

        function getNavigationHistory() {
            const history = getCookie('fileNavHistory');
            return history ? JSON.parse(history) : [];
        }

        function addToHistory(path) {
            let history = getNavigationHistory();
            // Remove if already exists to avoid duplicates
            history = history.filter(p => p !== path);
            // Add to end
            history.push(path);
            // Keep only last 20 items
            if (history.length > 20) history.shift();
            setCookie('fileNavHistory', JSON.stringify(history));
        }

        function updateBreadcrumb() {
            const breadcrumb = document.querySelector('.breadcrumb');
            const currentPath = getCurrentPath();
            breadcrumb.innerHTML = '';

            // Home link
            const homeLink = document.createElement('a');
            homeLink.textContent = '🏠';
            homeLink.style.cursor = 'pointer';
            homeLink.title = 'Home';
            homeLink.onclick = (e) => {
                e.preventDefault();
                if (globalHomePath) {
                    const homePath = globalHomePath.split('/').map(encodeURIComponent).join('/');
                    navigateTo(homePath);
                }
            };
            breadcrumb.appendChild(homeLink);

            // History dropdown
            const history = getNavigationHistory();
            if (history.length > 0) {
                const separator = document.createElement('span');
                separator.textContent = '|';
                breadcrumb.appendChild(separator);

                const historyBtn = document.createElement('a');
                historyBtn.textContent = '📋 Recent';
                historyBtn.style.position = 'relative';
                historyBtn.style.cursor = 'pointer';
                
                const dropdown = document.createElement('div');
                dropdown.style.display = 'none';
                dropdown.style.position = 'absolute';
                dropdown.style.top = '100%';
                dropdown.style.left = '0';
                dropdown.style.backgroundColor = 'var(--card-bg)';
                dropdown.style.border = '1px solid var(--border-color)';
                dropdown.style.borderRadius = '4px';
                dropdown.style.boxShadow = '0 2px 8px rgba(0, 0, 0, 0.15)';
                dropdown.style.zIndex = '1000';
                dropdown.style.minWidth = '200px';
                dropdown.style.maxHeight = '300px';
                dropdown.style.overflowY = 'auto';
                
                history.slice().reverse().forEach(path => {
                    const item = document.createElement('div');
                    item.style.padding = '8px 12px';
                    item.style.borderBottom = '1px solid var(--border-color)';
                    item.style.cursor = 'pointer';
                    item.style.whiteSpace = 'nowrap';
                    item.style.textOverflow = 'ellipsis';
                    item.style.overflow = 'hidden';
                    item.style.fontSize = '13px';
                    item.textContent = path || '/';
                    item.style.color = 'var(--text-main)';
                    
                    item.onmouseover = () => {
                        item.style.backgroundColor = 'var(--parent-hover)';
                    };
                    item.onmouseout = () => {
                        item.style.backgroundColor = 'transparent';
                    };
                    
                    item.onclick = () => {
                        navigateTo(path);
                        dropdown.style.display = 'none';
                    };
                    
                    dropdown.appendChild(item);
                });

                historyBtn.onmouseover = () => dropdown.style.display = 'block';
                historyBtn.onmouseout = () => dropdown.style.display = 'none';
                dropdown.onmouseover = () => dropdown.style.display = 'block';
                dropdown.onmouseout = () => dropdown.style.display = 'none';

                historyBtn.appendChild(dropdown);
                breadcrumb.appendChild(historyBtn);
            }
        }

        // File preview functionality
        const IMAGE_EXTS = /\.(jpg|jpeg|png|gif|webp|svg)$/i;
        const TEXT_EXTS = /\.(txt|log|csv|json|xml|yaml|yml|toml|ini|conf|config|env|md|py|js|go|java|c|cpp|h|py|hpp|sh|rb|php|ts|jsx|tsx|vue|css|html|sql)$/i;
        const PDF_EXTS = /\.(pdf)$/i;
        const MAX_PREVIEW_SIZE = 1024 * 1024; // 1MB limit

        function closePreview() {
            document.getElementById('previewModal').classList.remove('active');
        }

        function syntaxHighlight(text, ext) {
            // Simple syntax highlighting for common patterns
            let html = text
                .replace(/&/g, '&amp;')
                .replace(/</g, '&lt;')
                .replace(/>/g, '&gt;')
                .replace(/"/g, '&quot;');

            // Keywords
            const keywords = ['if', 'else', 'for', 'while', 'function', 'const', 'let', 'var', 'return', 'class', 'def', 'import', 'from', 'as', 'try', 'except', 'finally', 'with', 'yield', 'async', 'await', 'true', 'false', 'null', 'undefined', 'new', 'this', 'super', 'static', 'public', 'private', 'protected', 'package', 'interface', 'extends', 'implements', 'throws', 'synchronized'];
            keywords.forEach(kw => {
                html = html.replace(new RegExp(`\\b${kw}\\b`, 'g'), `<span class="syntax-keyword">${kw}</span>`);
            });

            // Strings (both single and double quotes)
            html = html.replace(/(["'`])(?:(?=(\\?))\2.)*?\1/g, '<span class="syntax-string">$&</span>');

            // Numbers
            html = html.replace(/\b\d+\.?\d*\b/g, '<span class="syntax-number">$&</span>');

            // Comments (# for Python/Bash, // for JS/Java, /* */ for multi-line)
            html = html.replace(/^#.*/gm, '<span class="syntax-comment">$&</span>');
            html = html.replace(/\/\/.*/g, '<span class="syntax-comment">$&</span>');

            return html;
        }

        async function previewFile(filePath, fileName) {
            // If open-in-new-window is enabled, just open the file normally
            if (openInNewWindow) {
                window.open(filePath, '_blank');
                return;
            }

            const modal = document.getElementById('previewModal');
            const title = document.getElementById('previewTitle');
            const content = document.getElementById('previewContent');

            title.textContent = fileName;
            content.innerHTML = '<p>Loading...</p>';
            modal.classList.add('active');

            try {
                const response = await fetch(filePath);
                if (!response.ok) throw new Error('Failed to load file');

                const blob = await response.blob();
                const size = blob.size;

                if (IMAGE_EXTS.test(fileName)) {
                    // Image preview
                    const url = URL.createObjectURL(blob);
                    content.innerHTML = `<img src="${url}" class="preview-image" alt="${fileName}">`;
                } else if (PDF_EXTS.test(fileName)) {
                    // PDF preview via the browser's built-in PDF viewer, no library needed
                    const pdfBlob = blob.type === 'application/pdf' ? blob : blob.slice(0, blob.size, 'application/pdf');
                    const url = URL.createObjectURL(pdfBlob);
                    content.innerHTML = `<iframe src="${url}" class="preview-pdf" title="${fileName}"></iframe>`;
                } else if (TEXT_EXTS.test(fileName)) {
                    // Text preview
                    let text = await blob.text();
                    const isTruncated = size > MAX_PREVIEW_SIZE;

                    if (isTruncated) {
                        text = text.substring(0, MAX_PREVIEW_SIZE);
                    }

                    const ext = fileName.split('.').pop().toLowerCase();
                    const highlighted = syntaxHighlight(text, ext);
                    const lines = highlighted.split('\n');
                    
                    const CHUNK_SIZE = 500;
                    let displayedLines = 0;

                    let html = '';
                    if (isTruncated) {
                        html += '<div class="preview-size-warning">File is large (' + 
                            (size / (1024 * 1024)).toFixed(2) + 'MB). Showing first 1MB.</div>';
                    }
                    
                    html += '<div class="preview-code" id="previewCode">';

                    // Load first chunk
                    for (let i = 0; i < Math.min(CHUNK_SIZE, lines.length); i++) {
                        const line = lines[i];
                        html += `<div class="preview-code-line">
                            <div class="preview-code-line-num">${i + 1}</div>
                            <div class="preview-code-line-content">${line || '&nbsp;'}</div>
                        </div>`;
                        displayedLines++;
                    }

                    html += '</div>';
                    content.innerHTML = html;

                    // Setup infinite scroll for remaining lines
                    if (displayedLines < lines.length) {
                        const previewCode = content.querySelector('#previewCode');
                        
                        const loadMore = () => {
                            const scrollTop = content.scrollTop;
                            const scrollHeight = content.scrollHeight;
                            const clientHeight = content.clientHeight;
                            
                            // Load more when user scrolls near bottom (within 200px)
                            if (scrollHeight - scrollTop - clientHeight < 200 && displayedLines < lines.length) {
                                let html = '';
                                for (let i = displayedLines; i < Math.min(displayedLines + CHUNK_SIZE, lines.length); i++) {
                                    const line = lines[i];
                                    html += `<div class="preview-code-line">
                                        <div class="preview-code-line-num">${i + 1}</div>
                                        <div class="preview-code-line-content">${line || '&nbsp;'}</div>
                                    </div>`;
                                }
                                previewCode.innerHTML += html;
                                displayedLines = Math.min(displayedLines + CHUNK_SIZE, lines.length);
                            }
                        };

                        content.addEventListener('scroll', loadMore);
                    }
                } else {
                    // Unsupported file type
                    const ext = fileName.split('.').pop().toLowerCase();
                    content.innerHTML = `<p>Cannot preview ${fileName}</p>
                        <p style="color: var(--text-secondary); font-size: 13px;">Type: ${ext}</p>
                        <p style="color: var(--text-secondary); font-size: 13px;">Size: ${(size / 1024).toFixed(2)} KB</p>`;
                }
            } catch (err) {
                content.innerHTML = `<p style="color: #d32f2f;">Error loading file: ${err.message}</p>`;
            }
        }

        // Encode parentheses and square brackets in paths: ( -> %28, ) -> %29, [ -> %5B, ] -> %5D
        function encodeParens(path) {
            return path.replace(/\(/g, '%28').replace(/\)/g, '%29').replace(/\[/g, '%5B').replace(/\]/g, '%5D');
        }

        let globalHomePath = null;
        let currentLoadedPath = null;

        function navigateTo(path) {
            if (path === getCurrentPath()) return;
            history.pushState({}, '', path);
            loadFiles();
        }

        function handleDirClick(e, path) {
            // let ctrl/cmd/shift/middle-click fall through to normal browser handling (open in new tab)
            if (e.button !== 0 || e.ctrlKey || e.metaKey || e.shiftKey) return;
            e.preventDefault();
            navigateTo(path);
        }

        window.addEventListener('popstate', () => {
            if (getCurrentPath() !== currentLoadedPath) loadFiles();
        });

        async function loadFiles() {
            currentLoadedPath = getCurrentPath();
            const dir = getCurrentPath();
            const encodedDir = encodeParens(dir);
            
            // Decode the path for display
            const displayDir = decodeURIComponent(dir) || '/';
            document.title = `Felix Stargate - ${displayDir}`;
            document.getElementById('title').textContent = displayDir;

            const response = await fetch('/api/listfiles', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ dir: encodedDir })
            });

            const data = await response.json();
            fileData = data.entries;
            globalHomePath = data.home;
            const homeBtn = document.getElementById('homeBtn');
            if (globalHomePath) {
                homeBtn.style.display = 'inline-block';
            }
            updateSlideshowButtonVisibility();

            fileFilterTerm = '';
            fileFilterInput.value = '';
            
            // Sort by name by default
            sortState.col = 'name';
            sortState.dir = 1;
            const sorted = [...fileData].sort((a, b) => a.name.localeCompare(b.name));
            currentSorted = sorted;

            updateSortIndicators('name');
            renderTable(sorted);
        }

        function formatFileSize(bytes) {
            if (bytes === 0) return '0 B';
            const k = 1024;
            const sizes = ['B', 'KB', 'MB', 'GB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return Math.round(bytes / Math.pow(k, i) * 100) / 100 + ' ' + sizes[i];
        }

        // Target window for file/link entries — driven by the "Open in new
        // window" toggle rather than a fixed file-type whitelist.
        function getFileTarget() {
            return openInNewWindow ? '_blank' : '_self';
        }

        function renderTable(entries) {
            const tbody = document.querySelector('#fileTable tbody');
            tbody.innerHTML = '';

            // Check if there are any hidden files and show/hide toggle accordingly
            const hasHiddenFiles = entries.some(e => e.name.startsWith('.'));
            document.getElementById('showHiddenToggle').parentElement.style.display = hasHiddenFiles ? '' : 'none';

            // Save current path to history
            addToHistory(getCurrentPath());

            entries = getFilteredEntries(entries);

            // Add parent directory link if not root
            const currentPath = getCurrentPath();
            const hasParentDir = currentPath !== '/' && currentPath !== '';
            
            if (hasParentDir) {
                const row = document.createElement('tr');
                row.classList.add('parent-dir');
                const parentPath = currentPath.substring(0, currentPath.lastIndexOf('/')) || '/';
                row.innerHTML = `
                    <td></td>
                    <td><a href="${parentPath}" onclick="handleDirClick(event, '${parentPath}')">..</a></td>
                    <td class="type-directory">directory</td>
                    <td class="size-placeholder">-</td>
                    <td class="size-placeholder">-</td>
                `;
                tbody.appendChild(row);
            }

            entries.forEach((entry) => {
                const row = document.createElement('tr');
                const entryPath = currentPath.endsWith('/') 
                    ? `${currentPath}${entry.name}`
                    : `${currentPath}/${entry.name}`;
                const encodedEntryPath = encodeParens(entryPath);

                let nameCell;
                if (entry.type === 'directory') {
                    nameCell = `<a href="${encodedEntryPath}" onclick="handleDirClick(event, '${encodedEntryPath}')">${entry.name}</a>`;
                } else {
                    // Check if file is previewable
                    const isPreviewable = IMAGE_EXTS.test(entry.name) || TEXT_EXTS.test(entry.name) || PDF_EXTS.test(entry.name);
                    if (isPreviewable) {
                        nameCell = `<a href="${encodedEntryPath}" target="${getFileTarget()}" onclick="if(event.button === 0 && !event.ctrlKey && !event.metaKey && !event.shiftKey) { previewFile('${encodedEntryPath}', '${entry.name}'); return false; }">${entry.name}</a>`;
                    } else if (isVideoFile(entry)) {
                        nameCell = `<a href="${encodedEntryPath}" target="${getFileTarget()}" onclick="if(event.button === 0 && !event.ctrlKey && !event.metaKey && !event.shiftKey) { openVideoFromTable('${encodedEntryPath}', '${entry.name}'); return false; }">${entry.name}</a>`;
                    } else if (isMusicPlayableFile(entry)) {
                        nameCell = `<a href="${encodedEntryPath}" target="${getFileTarget()}" onclick="if(event.button === 0 && !event.ctrlKey && !event.metaKey && !event.shiftKey) { openMusicFromTable('${encodedEntryPath}', '${entry.name}'); return false; }">${entry.name}</a>`;
                    } else {
                        nameCell = `<a href="${encodedEntryPath}" target="${getFileTarget()}">${entry.name}</a>`;
                    }
                }

                const typeClass = entry.type === 'directory' ? 'type-directory' : 
                                 entry.type === 'link' ? 'type-link' : '';
                const sizeDisplay = entry.type === 'file' ? formatFileSize(entry.size) : '-';
                const sizeClass = entry.type === 'file' ? '' : 'size-placeholder';

                const checkboxCell = entry.type === 'file' 
                    ? `<input type="checkbox" class="file-checkbox" data-filepath="${encodedEntryPath}" data-filename="${entry.name}" style="cursor: pointer;"></input>`
                    : '';

                row.innerHTML = `
                    <td style="text-align: center;">${checkboxCell}</td>
                    <td>${nameCell}</td>
                    <td class="${typeClass}">${entry.type}</td>
                    <td class="${sizeClass}">${sizeDisplay}</td>
                    <td class="size-placeholder">${new Date(entry.modified * 1000).toLocaleString()}</td>
                `;
                tbody.appendChild(row);
            });

            // Update breadcrumb after table is rendered
            updateBreadcrumb();
        }

        function sortByColumn(col) {
            if (sortState.col === col) {
                sortState.dir *= -1;
            } else {
                sortState.col = col;
                sortState.dir = 1;
            }

            const sorted = [...fileData].sort((a, b) => {
                let x = a[col];
                let y = b[col];

                if (col === 'size' || col === 'modified') {
                    return (x - y) * sortState.dir;
                }

                return x.localeCompare(y) * sortState.dir;
            });

            currentSorted = sorted;
            updateSortIndicators(col);
            renderTable(sorted);
        }

        function updateSortIndicators(col) {
            document.querySelectorAll('th').forEach(th => {
                th.classList.remove('sort-asc', 'sort-desc');
                if (th.dataset.col === col) {
                    th.classList.add(sortState.dir === 1 ? 'sort-asc' : 'sort-desc');
                }
            });
        }

        // Multi-file download functions
        async function downloadSelectedFiles() {
            const checkboxes = document.querySelectorAll('input.file-checkbox:checked');
            const selectedFiles = Array.from(checkboxes).map(cb => ({
                path: cb.getAttribute('data-filepath'),
                name: cb.getAttribute('data-filename')
            }));

            if (selectedFiles.length === 0) {
                alert('No files selected');
                return;
            }

            const downloadBtn = document.getElementById('downloadBtn');
            const originalText = downloadBtn.textContent;
            downloadBtn.disabled = true;

            for (let i = 0; i < selectedFiles.length; i++) {
                const file = selectedFiles[i];
                downloadBtn.textContent = `⬇ Downloading ${i + 1}/${selectedFiles.length}`;
                await downloadFile(file.path, file.name);
                if (i < selectedFiles.length - 1) {
                    await new Promise(resolve => setTimeout(resolve, 100));
                }
            }

            downloadBtn.disabled = false;
            downloadBtn.textContent = originalText;
        }

        async function downloadFile(filePath, fileName) {
            try {
                const response = await fetch(filePath);
                if (!response.ok) throw new Error(`HTTP ${response.status}`);
                
                const blob = await response.blob();
                const url = URL.createObjectURL(blob);
                const link = document.createElement('a');
                link.href = url;
                link.download = fileName;
                document.body.appendChild(link);
                link.click();
                document.body.removeChild(link);
                URL.revokeObjectURL(url);
            } catch (error) {
                console.error(`Failed to download ${fileName}:`, error);
            }
        }

        function toggleAllCheckboxes(checked) {
            document.querySelectorAll('input.file-checkbox').forEach(cb => {
                cb.checked = checked;
            });
            updateDownloadButtonVisibility();
        }

        function updateDownloadButtonVisibility() {
            const hasChecked = document.querySelectorAll('input.file-checkbox:checked').length > 0;
            const downloadBtn = document.getElementById('downloadBtn');
            const selectAllBtn = document.getElementById('selectAllBtn');
            downloadBtn.style.display = hasChecked ? 'inline-block' : 'none';
            selectAllBtn.style.display = document.querySelectorAll('input.file-checkbox').length > 0 ? 'inline-block' : 'none';
        }

        document.querySelectorAll('th[data-col]').forEach(th => {
            th.addEventListener('click', () => sortByColumn(th.dataset.col));
        });

        // Modal event listeners
        const previewModal = document.getElementById('previewModal');
        previewModal.addEventListener('click', (e) => {
            if (e.target === previewModal) {
                closePreview();
            }
        });

        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && previewModal.classList.contains('active')) {
                closePreview();
            }
        });

        // Initialize Home button before loading files
        const homeBtn = document.getElementById('homeBtn');
        homeBtn.onclick = () => {
            if (globalHomePath) {
                const homePath = globalHomePath.split('/').map(encodeURIComponent).join('/');
                navigateTo(homePath);
            }
        };

        // Initialize after all functions are defined
        updateBreadcrumb();
        loadFiles();

        // Download and checkbox event listeners
        document.getElementById('downloadBtn').addEventListener('click', downloadSelectedFiles);
        document.getElementById('selectAllBtn').addEventListener('click', () => {
            const allChecked = document.querySelectorAll('input.file-checkbox').length === 
                              document.querySelectorAll('input.file-checkbox:checked').length;
            toggleAllCheckboxes(!allChecked);
        });
        document.getElementById('selectAllCheckbox').addEventListener('change', (e) => {
            toggleAllCheckboxes(e.target.checked);
        });

        // Add event listeners to checkboxes when table is rendered
        const originalRenderTable = window.renderTable;
        window.renderTable = function(entries) {
            originalRenderTable(entries);
            document.querySelectorAll('input.file-checkbox').forEach(cb => {
                cb.addEventListener('change', updateDownloadButtonVisibility);
            });
            updateDownloadButtonVisibility();
        };

        // Check for warpdrive cookie and auto-open if set
        if (document.cookie.includes('warpdrive_active=true')) {
            setTimeout(() => {
                document.getElementById('consoleBtn').click();
            }, 500);
        }

        // Console functionality
        const consoleModal = document.createElement('div');
        consoleModal.className = 'console-modal';
        consoleModal.innerHTML = `
            <div class="console-container">
                <div class="console-header">
                    <h3>Felix Warpdrive</h3>
                    <button class="console-close">×</button>
                </div>
                <div class="console-output" id="consoleOutput"></div>
                <div class="console-input-line">
                    <span class="console-prompt">$ </span>
                    <input type="text" id="consoleInput" autofocus>
                </div>
            </div>
        `;
        document.body.appendChild(consoleModal);

        let currentPath = '/';
        let homePath = '/';
        let commandHistory = [];
        let historyIndex = -1;

        const consoleInput = document.getElementById('consoleInput');
        const consoleOutput = document.getElementById('consoleOutput');

        function formatBytes(bytes) {
            if (bytes === 0) return '0 B';
            const k = 1024;
            const sizes = ['B', 'KB', 'MB', 'GB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return (bytes / Math.pow(k, i)).toFixed(2) + ' ' + sizes[i];
        }

        function addOutputLine(text, isCommand = false) {
            const line = document.createElement('div');
            line.className = 'console-output-line';
            if (isCommand) {
                line.classList.add('command');
                line.textContent = '$ ' + text;
            } else {
                line.textContent = text;
            }
            consoleOutput.appendChild(line);
            consoleOutput.scrollTop = consoleOutput.scrollHeight;
        }

        function globToRegex(glob) {
            const escapeRegex = (str) => str.replace(/[.+^${}()|[\]\\]/g, '\\$&');
            let regex = '^';
            for (let i = 0; i < glob.length; i++) {
                const char = glob[i];
                if (char === '*') {
                    regex += '.*';
                } else if (char === '?') {
                    regex += '.';
                } else {
                    regex += escapeRegex(char);
                }
            }
            regex += '$';
            return new RegExp(regex, 'i');
        }

        async function downloadMultipleFiles(files, logger) {
            for (let i = 0; i < files.length; i++) {
                const file = files[i];
                logger(`[${i + 1}/${files.length}] Downloading: ${file.name}`);
                try {
                    const response = await fetch(file.path);
                    if (!response.ok) throw new Error(`HTTP ${response.status}`);
                    
                    const blob = await response.blob();
                    const url = URL.createObjectURL(blob);
                    const link = document.createElement('a');
                    link.href = url;
                    link.download = file.name;
                    document.body.appendChild(link);
                    link.click();
                    document.body.removeChild(link);
                    URL.revokeObjectURL(url);
                } catch (error) {
                    logger(`Error downloading ${file.name}: ${error.message}`);
                }
                if (i < files.length - 1) {
                    await new Promise(resolve => setTimeout(resolve, 100));
                }
            }
            logger('Download complete');
        }

        function resolvePath(pathStr) {
            // Remember if input had trailing slash
            const hadTrailingSlash = pathStr.endsWith('/');
            
            // Normalize path separators and split into components
            const parts = pathStr.split('/').filter(p => p !== '');
            const stack = [];
            
            for (const part of parts) {
                if (part === '..') {
                    if (stack.length > 0) {
                        stack.pop();
                    }
                    // If we're at root, .. stays at root (no error)
                } else if (part !== '.' && part !== '') {
                    stack.push(part);
                }
            }
            
            // Reconstruct path
            const resolved = '/' + stack.join('/');
            
            // Preserve trailing slash only if original had it
            if (hadTrailingSlash && !resolved.endsWith('/')) {
                return resolved + '/';
            }
            return resolved;
        }

        async function executeCommand(cmd) {
            cmd = cmd.trim();
            if (!cmd) return;

            addOutputLine(cmd, true);
            
            // Add to history (keep last 100)
            commandHistory.push(cmd);
            if (commandHistory.length > 100) {
                commandHistory.shift();
            }
            historyIndex = -1;

            // Parse command with support for quoted arguments
            const parts = [];
            let current = '';
            let inQuotes = false;
            let quoteChar = '';
            
            for (let i = 0; i < cmd.length; i++) {
                const char = cmd[i];
                if ((char === '"' || char === "'") && !inQuotes) {
                    inQuotes = true;
                    quoteChar = char;
                } else if (char === quoteChar && inQuotes) {
                    inQuotes = false;
                    quoteChar = '';
                } else if (char === ' ' && !inQuotes) {
                    if (current) {
                        parts.push(current);
                        current = '';
                    }
                } else {
                    current += char;
                }
            }
            if (current) {
                parts.push(current);
            }
            
            const command = parts[0];
            const args = parts.slice(1);

            try {
                if (command === 'pwd') {
                    addOutputLine(currentPath);
                } else if (command === 'ls') {
                    const pattern = args.join(' ') || '*';
                    let dir = currentPath;
                    let filename = pattern;
                    
                    // Check if pattern contains a directory separator
                    if (pattern.includes('/')) {
                        let targetDir = pattern.substring(0, pattern.lastIndexOf('/'));
                        dir = pattern.startsWith('/') ? targetDir || '/' : currentPath + (currentPath.endsWith('/') ? '' : '/') + targetDir;
                        filename = pattern.substring(pattern.lastIndexOf('/') + 1);
                    }
                    
                    // Normalize dir - remove trailing slash for consistency, then add it back for API call
                    if (dir.endsWith('/') && dir !== '/') {
                        dir = dir.slice(0, -1);
                    }
                    const apiDir = dir.endsWith('/') ? dir : dir + '/';
                    
                    const response = await fetch('/api/listfiles', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ dir: apiDir })
                    });
                    if (response.ok) {
                        const data = await response.json();
                        if (data.entries && data.entries.length > 0) {
                            const regex = new RegExp('^' + filename.replace(/\./g, '\\.').replace(/\*/g, '.*').replace(/\?/g, '.') + '$');
                            const matches = data.entries.filter(e => regex.test(e.name)).sort((a, b) => a.name.localeCompare(b.name));
                            
                            if (matches.length === 0) {
                                addOutputLine('(no matches)');
                            } else if (matches.length === 1 && matches[0].type === 'directory') {
                                // If single match is a directory, list its contents
                                let subDir = dir + (dir.endsWith('/') ? '' : '/') + matches[0].name;
                                // Normalize subDir - remove trailing slash for consistency, then add it back for API call
                                if (subDir.endsWith('/')) {
                                    subDir = subDir.slice(0, -1);
                                }
                                const apiSubDir = subDir.endsWith('/') ? subDir : subDir + '/';
                                const subResponse = await fetch('/api/listfiles', {
                                    method: 'POST',
                                    headers: { 'Content-Type': 'application/json' },
                                    body: JSON.stringify({ dir: apiSubDir })
                                });
                                if (subResponse.ok) {
                                    const subData = await subResponse.json();
                                    if (subData.entries && subData.entries.length > 0) {
                                        const sorted = [...subData.entries].sort((a, b) => a.name.localeCompare(b.name));
                                        sorted.forEach(entry => {
                                            const type = entry.type === 'directory' ? '/' : '';
                                            addOutputLine(entry.name + type);
                                        });
                                    } else {
                                        addOutputLine('(empty directory)');
                                    }
                                }
                            } else {
                                matches.forEach(entry => {
                                    const type = entry.type === 'directory' ? '/' : '';
                                    addOutputLine(entry.name + type);
                                });
                            }
                        } else {
                            addOutputLine('(empty directory)');
                        }
                    } else {
                        addOutputLine('Error: directory not found');
                    }
                } else if (command === 'get') {
                    if (args.length === 0) {
                        addOutputLine('Usage: get <filename>');
                    } else {
                        const filePath = args.join(' ').startsWith('/') ? args.join(' ') : currentPath + (currentPath.endsWith('/') ? '' : '/') + args.join(' ');
                        window.open(filePath, '_blank');
                        addOutputLine('Downloading: ' + args.join(' '));
                    }
                } else if (command === 'mget') {
                    if (args.length === 0) {
                        addOutputLine('Usage: mget <pattern>');
                    } else {
                        const pattern = args.join(' ');
                        const apiDir = currentPath.endsWith('/') ? currentPath : currentPath + '/';
                        
                        const response = await fetch('/api/listfiles', {
                            method: 'POST',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({ dir: apiDir })
                        });
                        
                        if (response.ok) {
                            const data = await response.json();
                            const regex = globToRegex(pattern);
                            const matchedFiles = (data.entries || [])
                                .filter(e => e.type === 'file' && regex.test(e.name))
                                .map(e => ({
                                    path: apiDir + e.name,
                                    name: e.name
                                }));
                            
                            if (matchedFiles.length === 0) {
                                addOutputLine('No files matched: ' + pattern);
                            } else {
                                addOutputLine(`Downloading ${matchedFiles.length} file(s)...`);
                                downloadMultipleFiles(matchedFiles, addOutputLine);
                            }
                        } else {
                            addOutputLine('Error: cannot read directory');
                        }
                    }
                } else if (command === 'cd') {
                    if (args.length === 0) {
                        currentPath = '/';
                    } else if (args[0] === '~') {
                        currentPath = homePath.endsWith('/') ? homePath : homePath + '/';
                    } else if (args[0].startsWith('~/')) {
                        const subpath = args[0].substring(2);
                        const newPath = homePath + (homePath.endsWith('/') ? '' : '/') + subpath;
                        const response = await fetch(newPath, {
                            method: 'HEAD'
                        });
                        if (response.ok) {
                            const fileType = response.headers.get('Felix-file-type');
                            if (fileType === 'dir') {
                                currentPath = newPath.endsWith('/') ? newPath : newPath + '/';
                            } else if (fileType === 'file') {
                                addOutputLine('Error: not a directory');
                            } else {
                                currentPath = newPath.endsWith('/') ? newPath : newPath + '/';
                            }
                        } else if (response.status === 403) {
                            addOutputLine('Error: permission denied');
                        } else {
                            addOutputLine('Error: directory not found');
                        }
                    } else {
                        // Resolve the path, handling .. and . components
                        const inputPath = args.join(' ');
                        let pathToResolve = inputPath.startsWith('/') ? inputPath : currentPath + (currentPath.endsWith('/') ? '' : '/') + inputPath;
                        const newPath = resolvePath(pathToResolve);
                        
                        const response = await fetch(newPath, {
                            method: 'HEAD'
                        });
                        if (response.ok) {
                            const fileType = response.headers.get('Felix-file-type');
                            if (fileType === 'dir') {
                                currentPath = newPath;
                            } else if (fileType === 'file') {
                                addOutputLine('Error: not a directory');
                            } else {
                                currentPath = newPath;
                            }
                        } else if (response.status === 403) {
                            addOutputLine('Error: permission denied');
                        } else {
                            addOutputLine('Error: directory not found');
                        }
                    }
                } else if (command === 'info') {
                    if (args.length === 0) {
                        addOutputLine('Usage: info <file|directory>');
                    } else {
                        const inputPath = args.join(' ');
                        let targetPath = inputPath.startsWith('/') ? inputPath : currentPath + (currentPath.endsWith('/') ? '' : '/') + inputPath;
                        // Resolve path to handle .. and .
                        targetPath = resolvePath(targetPath);
                        
                        const response = await fetch(targetPath, {
                            method: 'HEAD'
                        });
                        if (response.ok) {
                            const fileType = response.headers.get('Felix-file-type');
                            const mimeType = response.headers.get('Content-Type');
                            const size = response.headers.get('Content-Length');
                            const modified = response.headers.get('Last-Modified');
                            const fileCount = response.headers.get('Felix-file-count');
                            
                            if (fileType === 'dir') {
                                addOutputLine('Directory: ' + inputPath);
                                addOutputLine('  Path: ' + targetPath);
                                if (fileCount) {
                                    addOutputLine('  Files: ' + fileCount);
                                }
                            } else {
                                addOutputLine('File: ' + inputPath);
                                addOutputLine('  Path: ' + targetPath);
                                if (mimeType) {
                                    addOutputLine('  Type: ' + mimeType);
                                }
                                if (size) {
                                    addOutputLine('  Size: ' + formatBytes(parseInt(size)));
                                }
                                if (modified) {
                                    addOutputLine('  Modified: ' + new Date(modified).toLocaleString());
                                }
                            }
                        } else if (response.status === 404) {
                            addOutputLine('Error: file or directory not found');
                        } else if (response.status === 403) {
                            addOutputLine('Error: permission denied');
                        } else {
                            addOutputLine('Error: unable to get file info');
                        }
                    }
                } else if (command === 'play') {
                    if (args.length === 0) {
                        addOutputLine('Usage: play <filename>');
                    } else {
                        const filePath = args.join(' ').startsWith('/') ? args.join(' ') : currentPath + (currentPath.endsWith('/') ? '' : '/') + args.join(' ');
                        const filename = args.join(' ');
                        
                        // Check for video formats (open in new tab)
                        const isVideo = /\.(mp4|webm|mkv|avi|mov)$/i.test(filename);
                        
                        // Check for image formats (open in new tab)
                        const isImage = /\.(jpg|jpeg|png|gif|webp)$/i.test(filename);
                        
                        // Check for audio formats (keep in console)
                        const isNativeAudio = /\.(mp3|wav|ogg|m4a|aac|flac)$/i.test(filename);
                        const isConvertibleAudio = /\.(aiff|aif|ra|au|voc|wma|m4b|ape|ac3|mp2|mid)$/i.test(filename);

                        // CD+G karaoke packs (zip) and KFN karaoke files use the full
                        // music overlay (CD+G canvas / synced lyrics), not the plain
                        // inline <audio> player.
                        const isKaraokeZip = /\.zip$/i.test(filename);
                        const isKfn = /\.kfn$/i.test(filename);
                        
                        const isPreviewable = /\.(txt|log|csv|json|xml|yaml|yml|toml|ini|conf|config|env|md|py|js|go|java|c|cpp|h|py|hpp|sh|rb|php|ts|jsx|tsx|vue|css|html|sql|pdf)$/i.test(filename);
                        if (isVideo || isImage) {
                            checkFileExists(filePath, filename, isImage);
                        } else if (isNativeAudio) {
                            playAudio(filePath, filename);
                        } else if (isConvertibleAudio) {
                            playWithConverter(filePath, filename, getConverterHeader(filename));
                        } else if (isKaraokeZip || isKfn) {
                            addOutputLine('Attempting to play karaoke file.');
                            playKaraokeTrack(filePath, filename, isKaraokeZip ? 'zip' : 'kfn');
                        } else if (isPreviewable) {
                            previewFile(filePath, filename);
                            addOutputLine('Previewing: ' + filename);
                        } else {
                            addOutputLine('Error: File type not playable/previewable');
                        }
                    }
                } else if (command === 'help') {
                    addOutputLine('Commands:');
                    addOutputLine('  ls [dir]       - List directory contents (sorted)');
                    addOutputLine('  pwd            - Print working directory');
                    addOutputLine('  cd [dir]       - Change directory (cd .. to go up, cd ~ for home)');
                    addOutputLine('  get <file>     - Download single file');
                    addOutputLine('  mget <pattern> - Download multiple files (e.g., mget *.mp3)');
                    addOutputLine('  play <file>    - Play or preview file (converts aiff, ra, midi, voc, mp2, and a few other formats)');
                    addOutputLine('                       Images, text, and movies files open in an overlay; music is in-line');
                    addOutputLine('                       CD+G packs (.zip) and .kfn karaoke files open in the music overlay');
                    addOutputLine('  info <file>    - Show file/directory info (size, type, modified date, file count)');
                    addOutputLine('  help           - Show this help');
                    addOutputLine('  clear          - Clear screen');
                    addOutputLine('  exit, bye      - Close console');
                } else if (command === 'clear') {
                    consoleOutput.innerHTML = '';
                } else if (command === 'exit' || command === 'bye') {
                    consoleModal.classList.remove('active');
                    // Clear cookie when warpdrive is closed
                    document.cookie = "warpdrive_active=; path=/; max-age=0";
                } else {
                    addOutputLine('Command not found: ' + command);
                }
            } catch (error) {
                addOutputLine('Error: ' + error.message);
            }

            consoleInput.value = '';
        }

        consoleInput.addEventListener('keypress', (e) => {
            if (e.key === 'Enter') {
                executeCommand(consoleInput.value);
            }
        });

        consoleInput.addEventListener('keydown', (e) => {
            if (e.key === 'ArrowUp') {
                e.preventDefault();
                if (historyIndex < commandHistory.length - 1) {
                    historyIndex++;
                    consoleInput.value = commandHistory[commandHistory.length - 1 - historyIndex];
                }
            } else if (e.key === 'ArrowDown') {
                e.preventDefault();
                if (historyIndex > 0) {
                    historyIndex--;
                    consoleInput.value = commandHistory[commandHistory.length - 1 - historyIndex];
                } else if (historyIndex === 0) {
                    historyIndex = -1;
                    consoleInput.value = '';
                }
            } else if (e.key === 'Tab') {
                e.preventDefault();
                autocompleteFilename();
            }
        });

        function getConverterHeader(filename) {
            if (/\.mid$/i.test(filename)) return 'X-Render-Midi';
            if (/\.aiff?$/i.test(filename)) return 'X-Render-Aiff';
            if (/\.au$/i.test(filename)) return 'X-Render-Au';
            if (/\.voc$/i.test(filename)) return 'X-Render-Voc';
            if (/\.ra$/i.test(filename)) return 'X-Convert-Audio';
            return 'X-Convert-Audio'; // Default for wma, m4b, ape, ac3, mp2
        }

        function checkFileExists(filePath, filename, isImage) {
            fetch(filePath, { method: 'HEAD' }).then(res => {
                if (res.ok) {
                    if (isImage) {
                        playImage(filePath, filename);
                    } else {
                        playVideo(filePath, filename);
                    }
                } else if (res.status === 404) {
                    addOutputLine('Error: File not found');
                } else {
                    addOutputLine(`Error: HTTP ${res.status}`);
                }
            }).catch(err => {
                addOutputLine('Error: Failed to open ' + filename);
                console.error(err);
            });
        }

        function playImage(filePath, filename) {
            try {
                addOutputLine('Viewing: ' + filename);
                
                // Create overlay
                const overlay = document.createElement('div');
                overlay.style.cssText = 'position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.8); display: flex; align-items: center; justify-content: center; z-index: 10000; cursor: pointer;';
                
                const img = document.createElement('img');
                img.src = filePath;
                img.style.cssText = 'max-width: 90%; max-height: 90%; object-fit: contain; cursor: auto;';
                
                overlay.appendChild(img);
                document.body.appendChild(overlay);
                
                // Close on click
                overlay.addEventListener('click', () => overlay.remove());
                
                // Close on escape key
                const closeOnEscape = (e) => {
                    if (e.key === 'Escape') {
                        overlay.remove();
                        document.removeEventListener('keydown', closeOnEscape);
                    }
                };
                document.addEventListener('keydown', closeOnEscape);
            } catch (error) {
                addOutputLine('Error: Failed to open ' + filename);
                console.error(error);
            }
        }

        function playVideo(filePath, filename) {
            try {
                addOutputLine('Playing: ' + filename);
                
                // Create overlay
                const overlay = document.createElement('div');
                overlay.style.cssText = 'position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.9); display: flex; align-items: center; justify-content: center; z-index: 10000; cursor: pointer;';
                
                const video = document.createElement('video');
                video.src = filePath;
                video.style.cssText = 'max-width: 90%; max-height: 90%; cursor: auto;';
                video.controls = true;
                video.autoplay = true;
                
                overlay.appendChild(video);
                document.body.appendChild(overlay);
                
                // Close on click (outside video)
                overlay.addEventListener('click', (e) => {
                    if (e.target === overlay) overlay.remove();
                });
                
                // Close on escape key
                const closeOnEscape = (e) => {
                    if (e.key === 'Escape') {
                        overlay.remove();
                        document.removeEventListener('keydown', closeOnEscape);
                    }
                };
                document.addEventListener('keydown', closeOnEscape);
            } catch (error) {
                addOutputLine('Error: Failed to open ' + filename);
                console.error(error);
            }
        }

        // Hands a .zip (CD+G pack) or .kfn file off to the full music overlay,
        // so it gets the same CD+G canvas / synced-lyrics rendering as tracks
        // played from the file browser's music player, instead of the plain
        // inline <audio> element used for ordinary audio files.
        function playKaraokeTrack(filePath, filename, type) {
            const encodedPath = filePath.split('/').map(part => encodeURIComponent(part)).join('/');
            musicTracks = [{ name: filename, src: encodedPath, type, id: 0, hasCdg: type === 'zip' ? null : false }];
            currentTrackId = null;
            musicSearchTerm = '';
            musicSearchInput.value = '';
            musicOverlay.classList.add('active');

            if (type === 'zip') {
                peekZipHasCdg(encodedPath).then(has => {
                    const track = musicTracks.find(t => t.id === 0);
                    if (track) {
                        track.hasCdg = has;
                        renderMusicTable();
                    }
                });
            }

            renderMusicTable();
            playTrackById(0);
        }

        function playAudio(filePath, filename) {
            try {
                addOutputLine('Playing: ' + filename);
                
                // URL encode the file path to handle special characters
                const encodedPath = filePath.split('/').map(part => encodeURIComponent(part)).join('/');
                
                // Create player element in console
                const playerDiv = document.createElement('div');
                playerDiv.style.cssText = 'margin: 10px 0; padding: 10px; background: var(--thead-bg); border-radius: 4px; border: 1px solid var(--border-color);';
                playerDiv.innerHTML = `
                    <div style="color: var(--text-main); margin-bottom: 8px; font-weight: bold;">${filename}</div>
                    <audio controls style="width: 100%; margin-top: 5px;">
                        <source src="${encodedPath}">
                        Your browser does not support the audio element.
                    </audio>
                `;
                consoleOutput.appendChild(playerDiv);
                consoleOutput.scrollTop = consoleOutput.scrollHeight;
                
                // Auto-play and handle errors
                const audio = playerDiv.querySelector('audio');
                audio.onerror = () => {
                    playerDiv.remove();
                    addOutputLine('Error: File not found');
                };
                audio.play().catch(e => {
                    console.error('Playback error:', e);
                });
            } catch (error) {
                addOutputLine('Error: Failed to play ' + filename);
                console.error(error);
            }
        }

        async function playWithConverter(filePath, filename, header) {
            try {
                addOutputLine('Converting and playing: ' + filename);
                const headers = { [header]: '1' };
                // URL encode the file path to handle special characters
                const encodedPath = filePath.split('/').map(part => encodeURIComponent(part)).join('/');
                const res = await fetch(encodedPath, { 
                    method: 'GET',
                    headers: headers 
                });
                if (!res.ok) {
                    if (res.status === 404) {
                        addOutputLine('Error: File not found');
                    } else {
                        addOutputLine(`Error: HTTP ${res.status}`);
                    }
                    return;
                }
                const blob = await res.blob();
                const audioUrl = URL.createObjectURL(blob);
                
                // Create player element in console
                const playerDiv = document.createElement('div');
                playerDiv.style.cssText = 'margin: 10px 0; padding: 10px; background: var(--thead-bg); border-radius: 4px; border: 1px solid var(--border-color);';
                playerDiv.innerHTML = `
                    <div style="color: var(--text-main); margin-bottom: 8px; font-weight: bold;">${filename}</div>
                    <audio controls style="width: 100%; margin-top: 5px;">
                        <source src="${audioUrl}" type="audio/wav">
                        Your browser does not support the audio element.
                    </audio>
                `;
                consoleOutput.appendChild(playerDiv);
                consoleOutput.scrollTop = consoleOutput.scrollHeight;
                
                // Auto-play
                const audio = playerDiv.querySelector('audio');
                audio.play().catch(e => {
                    console.error('Playback error:', e);
                });
            } catch (error) {
                addOutputLine('Error: Failed to convert/play ' + filename);
                console.error(error);
            }
        }

        async function autocompleteFilename() {
            const input = consoleInput.value;
            if (!input) return;

            // Split into command and arguments
            const trimmedInput = input.trim();
            const spaceIndex = trimmedInput.indexOf(' ');
            if (spaceIndex === -1) return; // No filename argument yet
            
            const command = trimmedInput.substring(0, spaceIndex);
            const filenameArg = trimmedInput.substring(spaceIndex + 1);
            
            // Determine directory and partial filename
            let dir = currentPath;
            let partial = filenameArg;
            
            // Handle quoted strings
            let isQuoted = false;
            let quoteChar = '';
            if ((filenameArg.startsWith('"') || filenameArg.startsWith("'"))) {
                isQuoted = true;
                quoteChar = filenameArg[0];
                partial = filenameArg.substring(1);
            }
            
            if (partial.includes('/')) {
                const lastSlash = partial.lastIndexOf('/');
                dir = partial.substring(0, lastSlash) || '/';
                partial = partial.substring(lastSlash + 1);
            }

            try {
                const response = await fetch('/api/listfiles', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ dir: dir })
                });
                
                if (response.ok) {
                    const data = await response.json();
                    const matches = data.entries
                        .filter(e => e.name.toLowerCase().startsWith(partial.toLowerCase()))
                        .sort((a, b) => a.name.localeCompare(b.name));

                    if (matches.length === 1) {
                        // Single match: complete it
                        const suffix = matches[0].type === 'directory' ? '/' : '';
                        let fullFilename = matches[0].name + suffix;
                        
                        // Reconstruct the path if we had a directory component
                        if (partial !== filenameArg) {
                            const dirPart = filenameArg.substring(0, filenameArg.lastIndexOf('/') + 1);
                            fullFilename = dirPart + fullFilename;
                        }
                        
                        // Add quotes if it has spaces or special chars and wasn't already quoted
                        if (!isQuoted && /[\s'"()[\]{}]/.test(fullFilename)) {
                            fullFilename = '"' + fullFilename + '"';
                        }
                        
                        const newValue = command + ' ' + fullFilename;
                        consoleInput.value = newValue;
                    } else if (matches.length > 1) {
                        // Multiple matches: find common prefix
                        let commonPrefix = matches[0].name;
                        for (let i = 1; i < matches.length; i++) {
                            let j = 0;
                            while (j < commonPrefix.length && j < matches[i].name.length && 
                                   commonPrefix[j].toLowerCase() === matches[i].name[j].toLowerCase()) {
                                j++;
                            }
                            commonPrefix = commonPrefix.substring(0, j);
                        }

                        if (commonPrefix.length > partial.length) {
                            let completion = (partial !== filenameArg ? filenameArg.substring(0, filenameArg.lastIndexOf('/') + 1) : '') + commonPrefix;
                            const newValue = command + ' ' + completion;
                            consoleInput.value = newValue;
                        } else {
                            // Show matches
                            addOutputLine(matches.map(m => m.name + (m.type === 'directory' ? '/' : '')).join('  '));
                        }
                    }
                }
            } catch (error) {
                addOutputLine('Error: ' + error.message);
            }
        }

        document.getElementById('consoleBtn').addEventListener('click', () => {
            consoleModal.classList.add('active');
            consoleInput.focus();
            homePath = globalHomePath || '/';
            currentPath = getCurrentPath();
            if (!currentPath.endsWith('/')) currentPath += '/';
            addOutputLine('Type "help" for commands');
            // Set cookie when warpdrive is opened
            document.cookie = "warpdrive_active=true; path=/; max-age=" + (365 * 24 * 60 * 60);
        });

        consoleModal.querySelector('.console-close').addEventListener('click', () => {
            consoleModal.classList.remove('active');
            // Clear cookie when warpdrive is closed
            document.cookie = "warpdrive_active=; path=/; max-age=0";
        });

        consoleModal.addEventListener('click', (e) => {
            if (e.target === consoleModal) {
                consoleModal.classList.remove('active');
                // Clear cookie when warpdrive is closed
                document.cookie = "warpdrive_active=; path=/; max-age=0";
            }
        });
