        let zoomScale = 1;
        let panX = 0;
        let panY = 0;
        const MIN_ZOOM = 1;
        const MAX_ZOOM = 6;

        function applyZoomTransform() {
            slideshowImg.style.transform = `translate(${panX}px, ${panY}px) scale(${zoomScale})`;
            slideshowImg.classList.toggle('zoomed', zoomScale > 1);
        }

        function resetZoom() {
            zoomScale = 1;
            panX = 0;
            panY = 0;
            applyZoomTransform();
        }

        function showSlide(index) {
            if (slideshowImages.length === 0) return;
            slideshowIndex = (index + slideshowImages.length) % slideshowImages.length;
            const img = slideshowImages[slideshowIndex];
            slideshowImg.src = img.src;
            slideshowImg.alt = img.name;
            slideshowCaption.textContent = `${img.name} (${slideshowIndex + 1}/${slideshowImages.length})`;
            resetZoom();
        }

        function openSlideshow() {
            slideshowImages = getImageEntries();
            if (slideshowImages.length === 0) return;
            slideshowOverlay.classList.add('active');
            showSlide(0);
            stopSlideshowAutoplay();
            history.pushState({ ivOverlay: 'slideshow' }, '');
        }

        function closeSlideshow(fromPopState) {
            slideshowOverlay.classList.remove('active');
            slideshowImg.src = '';
            resetZoom();
            stopSlideshowAutoplay();
            if (!fromPopState && history.state && history.state.ivOverlay === 'slideshow') {
                history.back();
            }
        }

        // ---- autoplay ----