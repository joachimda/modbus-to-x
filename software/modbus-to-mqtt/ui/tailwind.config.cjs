module.exports = {
    content: [
        "../data/**/*.html",
        "../data/**/*.js"
    ],
    darkMode: ['class', '[data-theme="dark"]'],
    theme: {
        extend: {
            colors: {
                brand: { DEFAULT: '#0ea5b7', dark: '#0891a2' }
            },
            borderRadius: { xl2: '1rem' }
        }
    },
    corePlugins: {
        preflight: true
    }
}