import React, { useState } from 'react';

interface CopyButtonProps {
  value: string;
  label?: string;
  onCopied?: (message: string) => void;
}

export const CopyButton: React.FC<CopyButtonProps> = ({ value, label = 'Copy', onCopied }) => {
  const [copied, setCopied] = useState(false);

  const handleCopy = async () => {
    try {
      await navigator.clipboard.writeText(value);
      setCopied(true);
      onCopied?.(`${label} copied`);
      window.setTimeout(() => setCopied(false), 1200);
    } catch {
      onCopied?.('Copy failed');
    }
  };

  return (
    <button
      type="button"
      title={`Copy ${value}`}
      onClick={handleCopy}
      className="inline-flex h-7 w-7 shrink-0 items-center justify-center rounded-md border border-border-slate bg-deck-navy text-text-secondary transition hover:bg-card-highlight hover:text-text-primary"
    >
      <span className="sr-only">{label}</span>
      {copied ? '✓' : '⧉'}
    </button>
  );
};
