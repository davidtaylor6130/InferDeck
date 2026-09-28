import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import { CheckCircleIcon, ExclamationTriangleIcon, InformationCircleIcon, XCircleIcon, XMarkIcon } from '@heroicons/react/20/solid';
import type { Tone } from '../types';

export interface ConfirmOptions {
  title: string;
  detail?: string;
  confirmLabel: string;
  destructive?: boolean;
}

interface Toast {
  id: number;
  message: string;
  detail?: string;
  tone: Tone;
}

interface FeedbackValue {
  toast: (message: string, options?: { tone?: Tone; detail?: string }) => void;
  confirm: (options: ConfirmOptions) => Promise<boolean>;
}

const fallback: FeedbackValue = {
  toast: () => {},
  confirm: async options => typeof window !== 'undefined' && window.confirm(`${options.title}${options.detail ? `\n\n${options.detail}` : ''}`),
};

const FeedbackContext = createContext<FeedbackValue>(fallback);
export const useFeedback = () => useContext(FeedbackContext);

const ICONS: Partial<Record<Tone, React.ComponentType<React.SVGProps<SVGSVGElement>>>> = {
  good: CheckCircleIcon,
  warn: ExclamationTriangleIcon,
  critical: XCircleIcon,
};
const ICON_TONE: Partial<Record<Tone, string>> = {
  good: 'text-success-green',
  warn: 'text-warning-amber',
  critical: 'text-danger-rose',
};

export const FeedbackProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [toasts, setToasts] = useState<Toast[]>([]);
  const [pending, setPending] = useState<(ConfirmOptions & { resolve: (value: boolean) => void }) | null>(null);
  const nextId = useRef(1);

  const dismiss = useCallback((id: number) => setToasts(current => current.filter(item => item.id !== id)), []);

  const toast = useCallback<FeedbackValue['toast']>((message, options = {}) => {
    const id = nextId.current++;
    setToasts(current => [{ id, message, detail: options.detail, tone: options.tone ?? 'good' }, ...current].slice(0, 3));
    window.setTimeout(() => dismiss(id), options.tone === 'critical' ? 8000 : 4500);
  }, [dismiss]);

  const confirm = useCallback<FeedbackValue['confirm']>(options => new Promise(resolve => {
    setPending({ ...options, resolve });
  }), []);

  const settle = (value: boolean) => {
    pending?.resolve(value);
    setPending(null);
  };

  const value = useMemo(() => ({ toast, confirm }), [toast, confirm]);

  return (
    <FeedbackContext.Provider value={value}>
      {children}
      <div className="pointer-events-none fixed inset-x-0 bottom-[calc(5rem+env(safe-area-inset-bottom,0px))] z-[60] flex flex-col items-center gap-2 px-4 md:bottom-6 md:right-6 md:left-auto md:items-end" role="status" aria-live="polite">
        {toasts.map(item => {
          const Icon = ICONS[item.tone] ?? InformationCircleIcon;
          return (
            <div key={item.id} className="pointer-events-auto flex w-full max-w-sm animate-toast-in items-start gap-2.5 rounded-lg border border-border-slate bg-panel-slate px-3.5 py-3 shadow-deck">
              <Icon className={`mt-0.5 h-4 w-4 shrink-0 ${ICON_TONE[item.tone] ?? 'text-queue-blue'}`} aria-hidden="true" />
              <div className="min-w-0 flex-1">
                <p className="text-sm font-medium text-text-primary">{item.message}</p>
                {item.detail && <p className="mt-0.5 break-words text-xs text-text-muted">{item.detail}</p>}
              </div>
              <button type="button" aria-label="Dismiss" onClick={() => dismiss(item.id)} className="-mr-1 rounded p-0.5 text-text-muted hover:text-text-primary">
                <XMarkIcon className="h-4 w-4" aria-hidden="true" />
              </button>
            </div>
          );
        })}
      </div>
      {pending && <ConfirmSheet options={pending} onSettle={settle} />}
    </FeedbackContext.Provider>
  );
};

const ConfirmSheet: React.FC<{ options: ConfirmOptions; onSettle: (value: boolean) => void }> = ({ options, onSettle }) => {
  const confirmRef = useRef<HTMLButtonElement>(null);
  const cancelRef = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    (options.destructive ? cancelRef : confirmRef).current?.focus();
    const onKey = (event: KeyboardEvent) => { if (event.key === 'Escape') onSettle(false); };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [options.destructive, onSettle]);

  return (
    <div
      className="fixed inset-0 z-[70] flex animate-fade-in items-end justify-center bg-black/40 p-3 sm:items-center"
      onMouseDown={event => { if (event.target === event.currentTarget) onSettle(false); }}
    >
      <div role="alertdialog" aria-modal="true" aria-labelledby="confirm-title" aria-describedby={options.detail ? 'confirm-detail' : undefined} className="w-full max-w-sm animate-sheet-up rounded-lg bg-panel-slate p-5 shadow-deck">
        <h2 id="confirm-title" className="text-base font-semibold text-text-primary">{options.title}</h2>
        {options.detail && <p id="confirm-detail" className="mt-1.5 text-sm text-text-secondary">{options.detail}</p>}
        <div className="mt-5 flex flex-col-reverse gap-2 sm:flex-row sm:justify-end">
          <button ref={cancelRef} type="button" onClick={() => onSettle(false)} className="min-h-10 rounded-md border border-line-strong px-4 text-sm font-medium text-text-primary hover:bg-elevated-slate sm:min-h-9">Cancel</button>
          <button
            ref={confirmRef}
            type="button"
            onClick={() => onSettle(true)}
            className={`min-h-10 rounded-md px-4 text-sm font-semibold sm:min-h-9 ${options.destructive ? 'bg-danger-rose text-white hover:bg-danger-rose/90' : 'bg-queue-blue text-on-accent hover:bg-queue-blue/90'}`}
          >
            {options.confirmLabel}
          </button>
        </div>
      </div>
    </div>
  );
};
