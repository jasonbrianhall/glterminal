        const THEMES = [
            { id: 'default', name: 'Default (Purple)' },
            { id: 'ocean', name: 'Ocean' },
            { id: 'sunset', name: 'Sunset' },
            { id: 'forest', name: 'Forest' },
            { id: 'rose', name: 'Rose' },
            { id: 'autumn', name: 'Autumn' },
            { id: 'mint', name: 'Mint' },
            { id: 'grape', name: 'Grape' },
            { id: 'steel', name: 'Steel' },
            { id: 'coffee', name: 'Coffee' },
            { id: 'candy', name: 'Candy' },
            { id: 'sky', name: 'Sky' },
            { id: 'lime', name: 'Lime' },
            { id: 'graphite', name: 'Graphite' },
            { id: 'midnight', name: 'Midnight' },
            { id: 'dracula', name: 'Dracula' },
            { id: 'nord', name: 'Nord' },
            { id: 'monokai', name: 'Monokai' },
            { id: 'cyberpunk', name: 'Cyberpunk' },
            { id: 'highcontrast', name: 'High Contrast' },
            { id: 'slate-blue', name: 'Slate Blue' },
        ];

        const themeSelect = document.getElementById('themeSelect');
        themeSelect.innerHTML = THEMES.map(t => `<option value="${t.id}">${t.name}</option>`).join('');

        function applyTheme(id) {
            if (id === 'default') {
                document.documentElement.removeAttribute('data-theme');
            } else {
                document.documentElement.setAttribute('data-theme', id);
            }
        }

        const storedTheme = getCookie('theme') || 'default';
        applyTheme(storedTheme);
        themeSelect.value = storedTheme;

        themeSelect.addEventListener('change', () => {
            applyTheme(themeSelect.value);
            setCookie('theme', themeSelect.value, 400);
        });

        let currentSorted = [];