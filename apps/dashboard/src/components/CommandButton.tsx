import React from 'react';

interface CommandButtonProps extends React.ButtonHTMLAttributes<HTMLButtonElement> {
  tone?: 'violet' | 'orange' | 'amber' | 'blue' | 'rose' | 'neutral' | 'green';
  children: React.ReactNode;
}

const tones = {
  violet: 'border-infer-violet/80 bg-infer-violet/10 text-text-primary hover:bg-infer-violet/20',
  orange: 'border-gaming-orange/80 bg-gaming-orange/10 text-text-primary hover:bg-gaming-orange/20',
  amber: 'border-warning-amber/80 bg-warning-amber/10 text-text-primary hover:bg-warning-amber/20',
  blue: 'border-queue-blue/70 bg-queue-blue/10 text-text-primary hover:bg-queue-blue/20',
  rose: 'border-danger-rose/80 bg-danger-rose/10 text-text-primary hover:bg-danger-rose/20',
  green: 'border-success-green/70 bg-success-green/10 text-text-primary hover:bg-success-green/20',
  neutral: 'border-border-slate bg-deck-navy text-text-primary hover:bg-card-highlight',
};

export const CommandButton: React.FC<CommandButtonProps> = ({ tone = 'neutral', className = '', children, ...props }) => (
  <button
    {...props}
    className={`inline-flex min-h-10 items-center justify-center gap-2 whitespace-nowrap rounded-lg border px-4 py-2 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50 ${tones[tone]} ${className}`}
  >
    {children}
  </button>
);
