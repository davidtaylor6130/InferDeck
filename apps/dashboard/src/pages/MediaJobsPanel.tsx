import React, { useEffect, useState } from 'react';
import { cancelMediaJob, getMediaJobs, type MediaJob } from '../api';
import { Badge, Button, Panel, ProgressBar, SectionTitle } from '../components/ui';

export const MediaJobsPanel: React.FC = () => {
  const [jobs, setJobs] = useState<MediaJob[]>([]);

  useEffect(() => {
    let active = true;
    const refresh = async () => {
      try {
        const result = await getMediaJobs();
        if (active) setJobs(result);
      } catch {}
    };
    void refresh();
    const timer = setInterval(() => { void refresh(); }, 1000);
    return () => { active = false; clearInterval(timer); };
  }, []);

  if (jobs.length === 0) return null;
  return (
    <Panel>
      <SectionTitle title="Media jobs" aside={`${jobs.length}`} />
      <div className="mt-3 space-y-2">
        {jobs.slice(0, 20).map(job => (
          <div key={job.id} className="rounded-md border border-white/10 bg-[#07101d] p-3">
            <div className="flex items-center justify-between gap-3">
              <span className="truncate text-xs text-text-primary">{job.model} · {job.modality}</span>
              <div className="flex items-center gap-2">
                <Badge label={job.state} tone={job.state === 'completed' ? 'good' : job.state === 'failed' ? 'critical' : job.state === 'cancelled' ? 'warn' : 'info'} />
                {job.state === 'running' && <Button tone="danger" onClick={() => { void cancelMediaJob(job.id); }}>Cancel</Button>}
              </div>
            </div>
            <div className="mt-2"><ProgressBar percent={job.progress} tone={job.state === 'failed' ? 'critical' : 'info'} /></div>
          </div>
        ))}
      </div>
    </Panel>
  );
};
