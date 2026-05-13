/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {
      colors: {
        'void-black': '#050814',
        'deck-navy': '#0B1220',
        'panel-slate': '#111827',
        'elevated-slate': '#162033',
        'card-highlight': '#1B2540',
        'border-slate': '#27344D',
        'text-primary': '#F8FAFC',
        'text-secondary': '#94A3B8',
        'text-muted': '#64748B',
        'infer-violet': '#8B5CF6',
        'ion-cyan': '#22D3EE',
        'queue-blue': '#60A5FA',
        'success-green': '#22C55E',
        'gpu-mint': '#34D399',
        'warning-amber': '#F59E0B',
        'gaming-orange': '#FB923C',
        'danger-rose': '#F43F5E',
      },
      fontFamily: {
        sans: ['Inter', 'ui-sans-serif', 'system-ui', 'sans-serif'],
        mono: ['ui-monospace', 'SFMono-Regular', 'Menlo', 'Monaco', 'Consolas', 'monospace'],
      },
      boxShadow: {
        deck: '0 18px 80px rgba(0, 0, 0, 0.35)',
      },
    },
  },
  plugins: [],
}
