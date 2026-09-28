import { useCallback, useEffect, useState } from 'react';

export type ThemePreference = 'light' | 'dark' | 'system';
export const THEME_STORAGE_KEY = 'inferdeck:theme';
const THEME_COLOR: Record<'light' | 'dark', string> = { light: '#f2f2f5', dark: '#1c1c1f' };

export function readThemePreference(): ThemePreference {
  try {
    const stored = localStorage.getItem(THEME_STORAGE_KEY);
    return stored === 'dark' || stored === 'system' ? stored : 'light';
  } catch {
    return 'light';
  }
}

export function resolveTheme(preference: ThemePreference, systemDark: boolean): 'light' | 'dark' {
  return preference === 'system' ? (systemDark ? 'dark' : 'light') : preference;
}

let crossfadeTimer: ReturnType<typeof setTimeout> | undefined;

function apply(preference: ThemePreference) {
  const dark = window.matchMedia?.('(prefers-color-scheme: dark)').matches ?? false;
  const theme = resolveTheme(preference, dark);
  const root = document.documentElement;
  if (root.dataset.theme && root.dataset.theme !== theme) {
    root.classList.add('theme-changing');
    clearTimeout(crossfadeTimer);
    crossfadeTimer = setTimeout(() => root.classList.remove('theme-changing'), 360);
  }
  root.dataset.theme = theme;
  document.querySelector('meta[name="theme-color"]')?.setAttribute('content', THEME_COLOR[theme]);
}

export function useTheme(): [ThemePreference, (next: ThemePreference) => void] {
  const [preference, setPreference] = useState<ThemePreference>(() =>
    typeof window === 'undefined' ? 'light' : readThemePreference());

  useEffect(() => {
    apply(preference);
    if (preference !== 'system') return;
    const query = window.matchMedia('(prefers-color-scheme: dark)');
    const onChange = () => apply('system');
    query.addEventListener('change', onChange);
    return () => query.removeEventListener('change', onChange);
  }, [preference]);

  const update = useCallback((next: ThemePreference) => {
    try {
      localStorage.setItem(THEME_STORAGE_KEY, next);
    } catch {
    }
    setPreference(next);
  }, []);

  return [preference, update];
}
