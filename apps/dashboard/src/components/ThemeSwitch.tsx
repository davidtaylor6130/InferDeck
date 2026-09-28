import React from 'react';
import { ComputerDesktopIcon, MoonIcon, SunIcon } from '@heroicons/react/24/outline';
import { useTheme, type ThemePreference } from '../theme';

const OPTIONS: Array<{ id: ThemePreference; label: string; Icon: React.ComponentType<React.SVGProps<SVGSVGElement>> }> = [
  { id: 'light', label: 'Light', Icon: SunIcon },
  { id: 'dark', label: 'Dark', Icon: MoonIcon },
  { id: 'system', label: 'Match system', Icon: ComputerDesktopIcon },
];

export const ThemeSwitch: React.FC<{ showLabels?: boolean }> = ({ showLabels }) => {
  const [theme, setTheme] = useTheme();
  return (
    <div role="radiogroup" aria-label="Appearance" className="flex rounded-lg bg-elevated-slate p-0.5">
      {OPTIONS.map(({ id, label, Icon }) => (
        <button
          key={id}
          type="button"
          role="radio"
          aria-checked={theme === id}
          aria-label={label}
          title={label}
          onClick={() => setTheme(id)}
          className={`flex min-h-8 flex-1 items-center justify-center gap-1.5 rounded-md text-sm font-medium ${theme === id ? 'bg-panel-slate text-text-primary shadow-card' : 'text-text-muted hover:text-text-primary'}`}
        >
          <Icon className="h-4 w-4" aria-hidden="true" />
          {showLabels && label}
        </button>
      ))}
    </div>
  );
};
