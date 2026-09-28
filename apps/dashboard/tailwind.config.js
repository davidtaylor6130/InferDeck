/** @type {import('tailwindcss').Config} */
const token = name => `rgb(var(--${name}) / <alpha-value>)`;

export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {
      colors: {
        'void-black': token('canvas'),
        'deck-navy': token('sidebar'),
        'panel-slate': token('surface'),
        'elevated-slate': token('fill'),
        'card-highlight': token('fill-strong'),
        'border-slate': token('separator'),
        'line-strong': token('line'),
        'text-primary': token('ink'),
        'text-secondary': token('ink-2'),
        'text-muted': token('ink-3'),
        'on-accent': token('on-accent'),
        'infer-violet': token('violet'),
        'ion-cyan': token('accent'),
        'queue-blue': token('accent'),
        'success-green': token('good'),
        'gpu-mint': token('good'),
        'warning-amber': token('warn'),
        'gaming-orange': token('warn'),
        'danger-rose': token('bad'),
        'series-1': token('series-1'),
        'series-2': token('series-2'),
      },
      fontFamily: {
        sans: ['"IBM Plex Sans"', '"Segoe UI"', 'system-ui', 'sans-serif'],
        mono: ['"IBM Plex Mono"', 'ui-monospace', 'Consolas', 'monospace'],
      },
      fontSize: {
        '2xs': ['11px', '16px'],
        xs: ['12px', '16px'],
        sm: ['13px', '20px'],
        base: ['14px', '20px'],
        lg: ['16px', '24px'],
        xl: ['20px', '28px'],
        '2xl': ['24px', '32px'],
      },
      borderRadius: {
        DEFAULT: '5px',
        md: '8px',
        lg: '12px',
      },
      boxShadow: {
        deck: 'var(--shadow-overlay)',
        card: 'var(--shadow-card)',
      },
      keyframes: {
        'page-in': { from: { opacity: '0', transform: 'translateY(calc(var(--motion-distance) * 8px))' }, to: { opacity: '1', transform: 'none' } },
        'item-in': { from: { opacity: '0', transform: 'translateY(calc(var(--motion-distance) * 6px))' }, to: { opacity: '1', transform: 'none' } },
        'sheet-up': { from: { opacity: '0', transform: 'translateY(calc(var(--motion-distance) * 24px)) scale(calc(1 - var(--motion-distance) * 0.02))' }, to: { opacity: '1', transform: 'none' } },
        'sheet-down': { from: { opacity: '1', transform: 'none' }, to: { opacity: '0', transform: 'translateY(calc(var(--motion-distance) * 16px))' } },
        'fade-in': { from: { opacity: '0' }, to: { opacity: '1' } },
        'fade-out': { from: { opacity: '1' }, to: { opacity: '0' } },
        'toast-in': { from: { opacity: '0', transform: 'translateY(calc(var(--motion-distance) * 16px)) scale(calc(1 - var(--motion-distance) * 0.04))' }, to: { opacity: '1', transform: 'none' } },
        'toast-out': { from: { opacity: '1', transform: 'none' }, to: { opacity: '0', transform: 'translateX(calc(var(--motion-distance) * 24px))' } },
      },
      animation: {
        'page-in': 'page-in 260ms cubic-bezier(0.2, 0.8, 0.2, 1) both',
        'item-in': 'item-in 320ms cubic-bezier(0.2, 0.8, 0.2, 1) both',
        'sheet-up': 'sheet-up 280ms cubic-bezier(0.3, 1.2, 0.4, 1) both',
        'sheet-down': 'sheet-down 180ms ease-in both',
        'fade-in': 'fade-in 200ms ease-out both',
        'fade-out': 'fade-out 180ms ease-in both',
        'toast-in': 'toast-in 320ms cubic-bezier(0.3, 1.25, 0.4, 1) both',
        'toast-out': 'toast-out 200ms ease-in both',
      },
    },
  },
  plugins: [],
}
