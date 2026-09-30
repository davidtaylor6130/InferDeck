/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {
      colors: {
        'void-black': '#000000',
        'deck-navy': '#000000',
        'panel-slate': '#111827',
        'elevated-slate': '#162033',
        'card-highlight': '#1B2540',
        'border-slate': '#2b3949',
        'text-primary': '#FFFFFF',
        'text-secondary': '#b0bcc8',
        'text-muted': '#7f8d9d',
        'infer-violet': '#6e8fb5',
        'ion-cyan': '#22D3EE',
        'queue-blue': '#72a7d8',
        'success-green': '#52b788',
        'gpu-mint': '#34D399',
        'warning-amber': '#d9a441',
        'gaming-orange': '#d68a45',
        'danger-rose': '#df6b72',
      },
      fontFamily: {
        sans: ['Segoe UI Variable', 'Segoe UI', 'system-ui', 'sans-serif'],
        mono: ['ui-monospace', 'SFMono-Regular', 'Menlo', 'Monaco', 'Consolas', 'monospace'],
      },
      boxShadow: {
        deck: '0 18px 80px rgba(0, 0, 0, 0.35)',
      },
    },
  },
  plugins: [],
}
