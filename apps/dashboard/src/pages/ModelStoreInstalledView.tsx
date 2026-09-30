import React, { useMemo } from 'react';
import type { InstalledStoreModel } from '../api';
import { Badge, Button, EmptyState, Panel, SectionTitle } from '../components/ui';
import { sectionLabel, type DashboardSection } from '../dashboardSections';
import { formatBytes } from '../utils';
import { serverModelType, storeInputClass, type ServerSortKey } from './modelStoreUi';

export const ModelStoreInstalledView: React.FC<{
  section: DashboardSection;
  entries: InstalledStoreModel[];
  sort: { key: ServerSortKey; direction: 'asc' | 'desc' };
  onSort: (sort: { key: ServerSortKey; direction: 'asc' | 'desc' }) => void;
  onRetire: (model: string, action: 'archive' | 'remove') => void;
  onUnregister: (model: string) => void;
}> = ({ section, entries, sort, onSort, onRetire, onUnregister }) => {
  const sorted = useMemo(() => {
    const value = (entry: InstalledStoreModel) => {
      if (sort.key === 'name') return entry.name || entry.path || '';
      if (sort.key === 'type') return serverModelType(entry);
      if (sort.key === 'configured') return entry.configured ? 1 : 0;
      if (sort.key === 'runtime') return entry.runtime || '';
      return entry.size ?? 0;
    };
    return [...entries].sort((left, right) => {
      const leftValue = value(left);
      const rightValue = value(right);
      const compared = typeof leftValue === 'number' && typeof rightValue === 'number'
        ? leftValue - rightValue
        : String(leftValue).localeCompare(
            String(rightValue), undefined, { numeric: true, sensitivity: 'base' },
          );
      return sort.direction === 'asc' ? compared : -compared;
    });
  }, [entries, sort]);

  return (
    <Panel className="border-t-0 pt-0">
      <SectionTitle title="Installed models" aside={`${entries.length} detected`} />
      <p className="mt-2 max-w-3xl text-xs text-text-muted">
        External models can be removed from InferDeck without deleting their files.
        Managed downloads can be archived or permanently deleted.
      </p>
      <div className="mt-4 flex flex-wrap items-end gap-2">
        <label className="text-xs text-text-muted">
          Sort by
          <select
            className={`${storeInputClass} mt-1`}
            value={sort.key}
            onChange={event => onSort({
              ...sort,
              key: event.target.value as ServerSortKey,
            })}
          >
            <option value="name">Name</option>
            <option value="type">Type</option>
            <option value="configured">Configured</option>
            <option value="runtime">Runtime</option>
            <option value="size">Disk size</option>
          </select>
        </label>
        <Button onClick={() => onSort({
          ...sort,
          direction: sort.direction === 'asc' ? 'desc' : 'asc',
        })}>
          {sort.direction === 'asc' ? 'Ascending' : 'Descending'}
        </Button>
      </div>
      {sorted.length === 0 ? (
        <div className="mt-4">
          <EmptyState
            title={`No downloaded ${sectionLabel(section)} models`}
            detail="No compatible artifacts were detected in this model library."
          />
        </div>
      ) : (
        <div className="mt-4 divide-y divide-white/10 border-y border-white/10">
          {sorted.map(entry => (
            <div
              key={entry.id || `${entry.name}:${entry.path}`}
              className="grid gap-3 py-3 md:grid-cols-[minmax(0,1fr)_minmax(220px,auto)] md:items-center"
            >
              <div className="min-w-0">
                <p className="break-all font-mono text-sm text-text-primary">
                  {entry.name || 'Unconfigured model artifact'}
                </p>
                <p className="mt-1 break-all text-xs text-text-muted">
                  {entry.path || 'Managed storage'}
                </p>
                <div className="mt-2 flex flex-wrap items-center gap-2 text-xs text-text-secondary">
                  <Badge label={serverModelType(entry)} tone={entry.hasVision ? 'violet' : 'idle'} />
                  <Badge
                    label={entry.configured ? 'Configured' : 'Not configured'}
                    tone={entry.configured ? 'good' : 'warn'}
                  />
                  <span>{entry.runtime || 'Unknown runtime'}</span>
                  <span>{formatBytes(entry.size ?? 0)}</span>
                  <span>{entry.managed ? 'InferDeck managed' : 'External'}</span>
                </div>
              </div>
              <div className="flex flex-wrap gap-2 md:justify-end">
                {entry.managed ? (
                  <>
                    <Button onClick={() => {
                      if (entry.name) onRetire(entry.name, 'archive');
                    }}>
                      Archive
                    </Button>
                    <Button tone="danger" onClick={() => {
                      if (entry.name) onRetire(entry.name, 'remove');
                    }}>
                      Delete permanently
                    </Button>
                  </>
                ) : entry.configured && entry.name ? (
                  <Button tone="danger" onClick={() => onUnregister(entry.name!)}>
                    Remove from InferDeck
                  </Button>
                ) : (
                  <span className="text-xs text-text-muted">Detected on disk</span>
                )}
              </div>
            </div>
          ))}
        </div>
      )}
    </Panel>
  );
};
