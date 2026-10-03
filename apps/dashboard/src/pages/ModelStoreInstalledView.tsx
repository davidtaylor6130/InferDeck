import React, { useMemo } from 'react';
import type { InstalledStoreModel } from '../api';
import { Badge, Button, EmptyState } from '../components/ui';
import { sectionLabel, type DashboardSection } from '../dashboardSections';
import { formatBytes } from '../utils';
import { serverModelType, type ServerSortKey } from './modelStoreUi';

const COLUMNS: Array<{ key: ServerSortKey; label: string; align?: 'right' }> = [
  { key: 'name', label: 'Model' },
  { key: 'type', label: 'Type' },
  { key: 'runtime', label: 'Runtime' },
  { key: 'configured', label: 'Status' },
  { key: 'size', label: 'Disk size', align: 'right' },
];

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
  const totalBytes = entries.reduce((sum, entry) => sum + (entry.size ?? 0), 0);

  return (
    <section aria-label="Installed models">
      <div className="flex flex-wrap items-baseline justify-between gap-3 pb-2">
        <h2 className="text-sm font-semibold">Installed models</h2>
        <span className="tabular text-xs text-text-muted">{entries.length} detected, {formatBytes(totalBytes)} on disk</span>
      </div>
      {sorted.length === 0 ? (
        <EmptyState
          title={`No downloaded ${sectionLabel(section)} models`}
          detail="No compatible model files were found in this model library."
        />
      ) : (
        <div className="overflow-x-auto rounded-lg border border-border-slate bg-panel-slate shadow-card">
          <table className="w-full min-w-[720px] border-collapse text-left text-sm">
            <thead>
              <tr className="border-b border-border-slate text-xs text-text-muted">
                {COLUMNS.map(column => {
                  const active = sort.key === column.key;
                  return (
                    <th key={column.key} className={`px-3 py-2 font-medium ${column.align === 'right' ? 'text-right' : ''}`} aria-sort={active ? (sort.direction === 'asc' ? 'ascending' : 'descending') : undefined}>
                      <button
                        type="button"
                        className={`hover:text-text-primary ${active ? 'text-text-primary' : ''}`}
                        onClick={() => onSort({ key: column.key, direction: active && sort.direction === 'asc' ? 'desc' : 'asc' })}
                      >
                        {column.label}{active ? (sort.direction === 'asc' ? ' ↑' : ' ↓') : ''}
                      </button>
                    </th>
                  );
                })}
                <th className="px-3 py-2"><span className="sr-only">Actions</span></th>
              </tr>
            </thead>
            <tbody>
              {sorted.map(entry => (
                <tr key={entry.id || `${entry.name}:${entry.path}`} className="border-b border-border-slate last:border-b-0">
                  <td className="max-w-0 px-3 py-2">
                    <div className="truncate font-mono text-text-primary">{entry.name || 'Unconfigured model file'}</div>
                    <div className="truncate text-xs text-text-muted" title={entry.path}>{entry.managed ? 'InferDeck managed' : 'External'}, {entry.path || 'managed storage'}</div>
                  </td>
                  <td className="whitespace-nowrap px-3 py-2 text-text-secondary">{serverModelType(entry)}</td>
                  <td className="whitespace-nowrap px-3 py-2 text-text-secondary">{entry.runtime || 'Unknown'}</td>
                  <td className="px-3 py-2"><Badge label={entry.configured ? 'Configured' : 'Not configured'} tone={entry.configured ? 'good' : 'warn'} /></td>
                  <td className="tabular whitespace-nowrap px-3 py-2 text-right text-text-secondary">{formatBytes(entry.size ?? 0)}</td>
                  <td className="whitespace-nowrap px-3 py-2 text-right">
                    {entry.managed ? (
                      <div className="flex justify-end gap-1.5">
                        <Button onClick={() => { if (entry.name) onRetire(entry.name, 'archive'); }} title={`Archive ${entry.name}`}>Archive</Button>
                        <Button tone="danger" onClick={() => { if (entry.name) onRetire(entry.name, 'remove'); }} title={`Delete ${entry.name} permanently`}>Delete</Button>
                      </div>
                    ) : entry.configured && entry.name ? (
                      <Button tone="danger" onClick={() => onUnregister(entry.name!)}>Remove from InferDeck</Button>
                    ) : (
                      <span className="text-xs text-text-muted">Detected on disk</span>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      <p className="mt-2 text-xs text-text-muted">
        Removing an external model from InferDeck keeps its files. Managed downloads can be archived or deleted permanently.
      </p>
    </section>
  );
};
