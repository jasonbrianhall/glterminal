        const slideshowAutoplayBtn = document.getElementById('slideshowAutoplay');
        let slideshowAutoplayHandle = null;

        function startSlideshowAutoplay() {
            if (slideshowAutoplayHandle) return;
            slideshowAutoplayHandle = setInterval(() => showSlide(slideshowIndex + 1), 2000);
            slideshowAutoplayBtn.classList.add('active');
            slideshowAutoplayBtn.innerHTML = '&#10074;&#10074;';
        }

        function stopSlideshowAutoplay() {
            if (slideshowAutoplayHandle) {
                clearInterval(slideshowAutoplayHandle);
                slideshowAutoplayHandle = null;
            }
            slideshowAutoplayBtn.classList.remove('active');
            slideshowAutoplayBtn.innerHTML = '&#9658;';
        }

        slideshowAutoplayBtn.addEventListener('click', () => {
            if (slideshowAutoplayHandle) stopSlideshowAutoplay();
            else startSlideshowAutoplay();
        });

        window.addEventListener('popstate', (e) => {
            if (slideshowOverlay.classList.contains('active') &&
                (!e.state || e.state.ivOverlay !== 'slideshow')) {
                closeSlideshow(true);
            }
        });

        slideshowImg.addEventListener('wheel', (e) => {
            e.preventDefault();
            const rect = slideshowImg.getBoundingClientRect();
            // Cursor position relative to image center, in unscaled pixels
            const cx = (e.clientX - rect.left - rect.width / 2) / zoomScale;
            const cy = (e.clientY - rect.top - rect.height / 2) / zoomScale;

            const zoomFactor = Math.exp(-e.deltaY * 0.0015);
            const newScale = Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, zoomScale * zoomFactor));
            const actualFactor = newScale / zoomScale;

            // Adjust pan so the point under the cursor stays fixed
            panX -= cx * (actualFactor - 1) * zoomScale;
            panY -= cy * (actualFactor - 1) * zoomScale;
            zoomScale = newScale;

            if (zoomScale === MIN_ZOOM) {
                panX = 0;
                panY = 0;
            }

            applyZoomTransform();
        }, { passive: false });

        // Drag to pan when zoomed in