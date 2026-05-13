import React from 'react';

interface SectionCardProps {
  title?: string;
  eyebrow?: string;
  action?: React.ReactNode;
  children: React.ReactNode;
  className?: string;
  bodyClassName?: string;
}

export const SectionCard: React.FC<SectionCardProps> = ({ title, eyebrow, action, children, className = '', bodyClassName = '' }) => (
  <section className={`rounded-xl border border-border-slate bg-panel-slate shadow-deck ${className}`}>
    {(title || eyebrow || action) && (
      <div className="flex min-w-0 flex-wrap items-center justify-between gap-3 border-b border-border-slate/70 px-4 py-3">
        <div className="min-w-0">
          {eyebrow && <p className="truncate text-[11px] uppercase tracking-wide text-text-muted">{eyebrow}</p>}
          {title && <h2 className="truncate text-lg font-semibold text-text-primary">{title}</h2>}
        </div>
        {action}
      </div>
    )}
    <div className={`p-4 ${bodyClassName}`}>{children}</div>
  </section>
);
