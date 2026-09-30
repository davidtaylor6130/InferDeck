import {
  DASHBOARD_SECTIONS,
  type DashboardSection,
} from './dashboardSections';

export const SIDEBAR_SECTION_STORAGE_KEY =
  'inferdeck:collapsed-sidebar-sections';

export function loadCollapsedSidebarSections(
  stored: string | null,
): DashboardSection[] {
  if (!stored) return [];
  try {
    const parsed = JSON.parse(stored) as unknown;
    if (!Array.isArray(parsed)) return [];
    const collapsed: DashboardSection[] = [];
    for (const value of parsed) {
      if (
        typeof value === 'string' &&
        DASHBOARD_SECTIONS.includes(value as DashboardSection) &&
        !collapsed.includes(value as DashboardSection)
      ) {
        collapsed.push(value as DashboardSection);
      }
    }
    return collapsed;
  } catch {
    return [];
  }
}

export function toggleCollapsedSidebarSection(
  current: readonly DashboardSection[],
  section: DashboardSection,
): DashboardSection[] {
  return current.includes(section)
    ? current.filter(value => value !== section)
    : [...current, section];
}
