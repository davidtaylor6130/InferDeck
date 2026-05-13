import React, { useState } from 'react';
import type { JobRecord, PageProps } from '../types';
import { EmptyState } from '../components/EmptyState';
import { JobDetailsDrawer } from '../components/JobDetailsDrawer';
import { QueueTable } from '../components/QueueTable';
import { SectionCard } from '../components/SectionCard';

export const JobsPage: React.FC<PageProps> = ({ state, actions }) => {
  const [selectedJob, setSelectedJob] = useState<JobRecord | null>(null);
  const history = state.jobsList.length ? state.jobsList : [];
  const selectJob = async (job: JobRecord) => {
    try {
      const [detailRes, eventsRes, resultRes] = await Promise.all([
        fetch(`/api/jobs/${encodeURIComponent(job.id)}`),
        fetch(`/api/jobs/${encodeURIComponent(job.id)}/events`),
        fetch(`/api/jobs/${encodeURIComponent(job.id)}/result`),
      ]);
      const detail = detailRes.ok ? await detailRes.json() : job;
      const events = eventsRes.ok ? await eventsRes.json() : { events: [] };
      const result = resultRes.ok ? await resultRes.json() : { result: detail.result };
      setSelectedJob({ ...job, ...detail, result: result.result ?? detail.result, events: events.events || [] });
    } catch {
      setSelectedJob(job);
    }
  };

  return (
    <div className="space-y-5">
      <SectionCard title="Job History" eyebrow="Metadata, payloads, results, errors">
        {history.length ? (
          <QueueTable jobs={history} onCancel={(id) => window.confirm(`Cancel running job ${id}?`) && actions.cancelJob(id)} onRetry={actions.retryJob} onSelect={selectJob} onCopied={actions.toast} />
        ) : (
          <EmptyState title="No job history yet." description="Completed jobs, payload previews, and event timelines will appear after traffic reaches the gateway." />
        )}
      </SectionCard>
      <JobDetailsDrawer job={selectedJob} onClose={() => setSelectedJob(null)} onCancel={actions.cancelJob} onRetry={actions.retryJob} onCopied={actions.toast} />
    </div>
  );
};
