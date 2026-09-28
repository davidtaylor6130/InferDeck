import React from 'react';
import { ComputerDesktopIcon, MoonIcon, SunIcon } from '@heroicons/react/24/outline';
import { useTheme, type ThemePreference } from '../theme';
import { SlidingIndicator, useSlidingIndicator } from './motion';

const OPTIONS: Array<{ id: ThemePreference; label: string; Icon: React.ComponentType<React.SVGProps<SVGSVGElement>> }> = [
  { id: 'light', label: 'Light', Icon: SunIcon },
  { id: 'dark', label: 'Dark', Icon: MoonIcon },
  { id: 'system', label: 'Match system', Icon: ComputerDesktopIcon },
];

export const ThemeSwitch: React.FC<{ showLabels?: boolean }> = ({ showLabels }) => {
  const [theme, setTheme] = useTheme();
  const { container, box, animated, ready } = useSlidingIndicator<HTMLDivElement>(theme);
  return (
    <div ref={container} role="radiogroup" aria-label="Appearance" className="relative flex rounded-lg bg-elevated-slate p-0.5">
      <SlidingIndicator box={box} animated={animated} />
      {OPTIONS.map(({ id, label, Icon }) => {
        const active = theme === id;
        return (
          <button
            key={id}
            type="button"
            role="radio"
            aria-checked={active}
            aria-label={label}
            title={label}
            data-active={active}
            onClick={() => setTheme(id)}
            className={`relative flex min-h-8 flex-1 items-center justify-center gap-1.5 rounded-md text-sm font-medium ${active ? `text-text-primary ${ready ? '' : 'bg-panel-slate shadow-card'}` : 'text-text-muted hover:text-text-primary'}`}
          >
            <Icon className={`h-4 w-4 transition-transform duration-300 ${active ? 'rotate-0 scale-110' : 'scale-100'}`} aria-hidden="true" />
            {showLabels && label}
          </button>
        );
      })}
    </div>
  );
};
