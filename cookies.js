        let fileData = [];
        let sortState = { col: null, dir: 1 };

        // ---- "open in new window" preference, persisted as a cookie ----
        function getCookie(name) {
            const match = document.cookie.match(new RegExp('(?:^|; )' + name + '=([^;]*)'));
            return match ? decodeURIComponent(match[1]) : null;
        }

        function setCookie(name, value, days) {
            const expires = new Date(Date.now() + days * 24 * 60 * 60 * 1000).toUTCString();
            document.cookie = `${name}=${encodeURIComponent(value)}; expires=${expires}; path=/; SameSite=Lax`;
        }

        const storedPref = getCookie('openInNewWindow');
        let openInNewWindow = storedPref === null ? true : storedPref === 'true';

        const newWindowToggle = document.getElementById('newWindowToggle');
        newWindowToggle.checked = openInNewWindow;
        newWindowToggle.addEventListener('change', () => {
            openInNewWindow = newWindowToggle.checked;
            setCookie('openInNewWindow', openInNewWindow, 400);
            // Re-render with the current sort so links pick up the new target immediately
            renderTable(currentSorted);
        });

        // ---- theme picker, persisted as a cookie ----