import React from 'react';
import { DASHBOARD_SECTIONS, modelsForSection, sectionLabel, type DashboardSection } from '../dashboardSections';
import { GENERATE_KINDS, type SectionArea } from '../routes';
import type { ModelInfo } from '../types';
import { SlidingIndicator, useSlidingIndicator } from './motion';

type Props =
  | { area: SectionArea; value: DashboardSection; models: ModelInfo[] }
  | { area: 'generate'; value: string; models: ModelInfo[] };

export const SectionSwitch: React.FC<Props> = ({ area, value, models }) => {
  const options: ReadonlyArray<DashboardSection> = area === 'generate' ? GENERATE_KINDS : DASHBOARD_SECTIONS;
  const showCounts = area === 'models' || area === 'store';
  const { container, box, animated, ready } = useSlidingIndicator<HTMLDivElement>(value);
  return (
    <nav aria-label="AI type" className="mb-6 flex max-w-full overflow-x-auto">
      <div ref={container} className="relative inline-flex gap-0.5 rounded-lg bg-elevated-slate p-0.5">
        <SlidingIndicator box={box} animated={animated} />
        {options.map(option => {
          const active = option === value;
          const count = modelsForSection(models, option).filter(model => !model.alias).length;
          return (
            <a
              key={option}
              href={`#${area}/${option}`}
              aria-current={active ? 'page' : undefined}
              data-active={active}
              className={`relative flex min-h-9 shrink-0 items-center gap-1.5 whitespace-nowrap rounded-md px-3 text-sm font-medium sm:min-h-7 ${active ? `text-text-primary ${ready ? '' : 'bg-panel-slate shadow-card'}` : 'text-text-secondary hover:text-text-primary'}`}
            >
              {sectionLabel(option)}
              {showCounts && <span className="tabular text-xs text-text-muted">{count}</span>}
            </a>
          );
        })}
      </div>
    </nav>
  );
};
