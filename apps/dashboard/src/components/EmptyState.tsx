import React from 'react';

interface EmptyStateProps {
  title: string;
  description?: string;
  action?: React.ReactNode;
}

export const EmptyState: React.FC<EmptyStateProps> = ({ title, description, action }) => (
  <div className="flex min-h-40 flex-col items-center justify-center rounded-lg border border-dashed border-border-slate bg-deck-navy/60 px-6 py-8 text-center">
    <div className="mb-3 h-2 w-16 rounded-full bg-gradient-to-r from-gpu-mint to-ion-cyan" />
    <h3 className="text-sm font-semibold text-text-primary">{title}</h3>
    {description && <p className="mt-1 max-w-md text-sm text-text-secondary">{description}</p>}
    {action && <div className="mt-4">{action}</div>}
  </div>
);
