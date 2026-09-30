import { describe, expect, it } from 'vitest';
import {
  loadCollapsedSidebarSections,
  toggleCollapsedSidebarSection,
} from './sidebarPreferences';

describe('sidebar section preferences', () => {
  it('loads only known section names and survives malformed storage', () => {
    expect(loadCollapsedSidebarSections('["llm","music","unknown","llm"]'))
      .toEqual(['llm', 'music']);
    expect(loadCollapsedSidebarSections('{broken')).toEqual([]);
    expect(loadCollapsedSidebarSections(null)).toEqual([]);
  });

  it('toggles each section independently without mutating the input', () => {
    const initial = ['llm'] as const;
    expect(toggleCollapsedSidebarSection(initial, 'music'))
      .toEqual(['llm', 'music']);
    expect(toggleCollapsedSidebarSection(initial, 'llm')).toEqual([]);
    expect(initial).toEqual(['llm']);
  });
});
