        const IMAGE_EXTENSIONS = ['jpg', 'jpeg', 'png', 'gif', 'webp', 'bmp', 'svg', 'avif', 'ico', 'tiff', 'tif'];

        function isImageFile(entry) {
            if (entry.type !== 'file') return false;
            const dot = entry.name.lastIndexOf('.');
            if (dot === -1) return false;
            const ext = entry.name.slice(dot + 1).toLowerCase();
            return IMAGE_EXTENSIONS.includes(ext);
        }

        function getImageEntries() {
            const currentPath = getCurrentPath();
            return fileData
                .filter(isImageFile)
                .sort((a, b) => a.name.localeCompare(b.name))
                .map(entry => {
                    const entryPath = currentPath.endsWith('/')
                        ? `${currentPath}${entry.name}`
                        : `${currentPath}/${entry.name}`;
                    return { name: entry.name, src: encodeParens(entryPath) };
                });
        }

        // ---- audio detection + music player ----