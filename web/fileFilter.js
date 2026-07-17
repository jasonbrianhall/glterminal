        let fileFilterTerm = '';
        let showHiddenFiles = false;

        const fileFilterInput = document.getElementById('fileFilter');
        fileFilterInput.addEventListener('input', () => {
            fileFilterTerm = fileFilterInput.value;
            renderTable(currentSorted);
        });

        const showHiddenToggle = document.getElementById('showHiddenToggle');
        showHiddenToggle.addEventListener('change', () => {
            showHiddenFiles = showHiddenToggle.checked;
            renderTable(currentSorted);
        });

        function getFilteredEntries(entries) {
            const term = fileFilterTerm.trim().toLowerCase();
            let filtered = entries;
            
            // Filter out hidden files if toggle is off
            if (!showHiddenFiles) {
                filtered = filtered.filter(e => !e.name.startsWith('.'));
            }
            
            // Apply search term
            if (!term) return filtered;
            return filtered.filter(e => e.name.toLowerCase().includes(term));
        }

        // Collapses repeated slashes (e.g. "//home/jbhall/Music" -> "/home/jbhall/Music")
        function getCurrentPath() {
            return window.location.pathname.replace(/\/+/g, '/');
        }

        // ---- image detection + slideshow ----