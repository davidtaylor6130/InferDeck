import React from 'react';
import { CopyButton } from './CopyButton';

interface LogViewerProps {
  lines: string[];
  onCopied?: (message: string) => void;
}

export const LogViewer: React.FC<LogViewerProps> = ({ lines, onCopied }) => (
  <div className="rounded-xl border border-border-slate bg-[#070B15]">
    <div className="flex items-center justify-between gap-3 border-b border-border-slate px-3 py-2">
      <p className="text-xs uppercase tracking-wide text-text-secondary">Log output</p>
      <CopyButton value={lines.join('\n')} label="Logs" onCopied={onCopied} />
    </div>
    <pre className="max-h-[560px] overflow-auto p-4 font-mono text-xs leading-6 text-text-secondary">
      {lines.join('\n')}
    </pre>
  </div>
);
