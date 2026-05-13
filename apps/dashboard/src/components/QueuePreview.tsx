import React from 'react';
import type { JobRecord } from '../types';
import { StatusBadge } from './StatusBadge';

interface QueuePreviewProps {
  jobs: JobRecord[];
}

export const QueuePreview: React.FC<QueuePreviewProps> = ({ jobs }) => (
  <div className="overflow-x-auto">
    <table className="min-w-[520px] w-full table-fixed text-left text-sm">
      <thead>
        <tr className="rounded-lg bg-card-highlight text-xs text-text-secondary">
          <th className="w-12 rounded-l-lg px-3 py-2 font-medium">Pos</th>
          <th className="w-28 px-3 py-2 font-medium">Type</th>
          <th className="w-20 px-3 py-2 font-medium">Priority</th>
          <th className="w-28 px-3 py-2 font-medium">Client</th>
          <th className="w-28 px-3 py-2 font-medium">Resource</th>
          <th className="w-24 rounded-r-lg px-3 py-2 font-medium">Status</th>
        </tr>
      </thead>
      <tbody className="divide-y divide-border-slate/70">
        {jobs.slice(0, 5).map((job, index) => (
          <tr key={job.id}>
            <td className="px-3 py-3">{index + 1}</td>
            <td className="truncate px-3 py-3">{job.type}</td>
            <td className="px-3 py-3">{job.priority}</td>
            <td className="truncate px-3 py-3">{job.client || 'Open WebUI'}</td>
            <td className="truncate px-3 py-3 font-mono text-xs text-text-secondary">{job.resourceClass || job.resource_class || 'gpu_llm'}</td>
            <td className="px-3 py-3"><StatusBadge label={job.status} tone={job.status} /></td>
          </tr>
        ))}
      </tbody>
    </table>
  </div>
);
