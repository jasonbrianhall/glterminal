        let isPanning = false;
        let panStartX = 0;
        let panStartY = 0;
        let panOriginX = 0;
        let panOriginY = 0;

        slideshowImg.addEventListener('mousedown', (e) => {
            if (zoomScale <= 1) return;
            isPanning = true;
            slideshowImg.classList.add('panning');
            panStartX = e.clientX;
            panStartY = e.clientY;
            panOriginX = panX;
            panOriginY = panY;
            e.preventDefault();
        });

        window.addEventListener('mousemove', (e) => {
            if (!isPanning) return;
            panX = panOriginX + (e.clientX - panStartX);
            panY = panOriginY + (e.clientY - panStartY);
            applyZoomTransform();
        });

        window.addEventListener('mouseup', () => {
            if (isPanning) {
                isPanning = false;
                slideshowImg.classList.remove('panning');
            }
        });

        // Double-click to toggle zoom
        slideshowImg.addEventListener('dblclick', () => {
            if (zoomScale > 1) {
                resetZoom();
            } else {
                zoomScale = 2;
                applyZoomTransform();
            }
        });

        slideshowBtn.addEventListener('click', openSlideshow);
        document.getElementById('slideshowClose').addEventListener('click', closeSlideshow);
        document.getElementById('slideshowPrev').addEventListener('click', () => showSlide(slideshowIndex - 1));
        document.getElementById('slideshowNext').addEventListener('click', () => showSlide(slideshowIndex + 1));

        slideshowOverlay.addEventListener('click', (e) => {
            if (e.target === slideshowOverlay) closeSlideshow();
        });

        document.addEventListener('keydown', (e) => {
            if (!slideshowOverlay.classList.contains('active')) return;
            if (e.key === 'Escape') closeSlideshow();
            else if (e.key === 'ArrowLeft') showSlide(slideshowIndex - 1);
            else if (e.key === 'ArrowRight') showSlide(slideshowIndex + 1);
        });

        // ---- minimal self-contained ZIP reader + DEFLATE inflater (no external deps) ----