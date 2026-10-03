import React, { createContext, useCallback, useContext, useEffect, useState } from 'react';
import { authenticateDashboard, getStatus, isAuthenticationError, logoutDashboard } from '../api';
import logoUrl from '../../../../Assets/Logo.png';

type AccessPhase = 'checking' | 'ready' | 'required' | 'offline';
interface DashboardAccessValue {
  remote: boolean;
  remembered: boolean | null;
  requireSignIn: () => void;
  logout: () => Promise<void>;
}
const AccessContext = createContext<DashboardAccessValue | null>(null);
export const useDashboardAccess = () => useContext(AccessContext);

export const DashboardAccess: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const remote = typeof window !== 'undefined' &&
    !['localhost', '127.0.0.1', '::1', '[::1]'].includes(window.location.hostname);
  const [phase, setPhase] = useState<AccessPhase>(remote ? 'checking' : 'ready');
  const [token, setToken] = useState('');
  const [remember, setRemember] = useState(true);
  const [remembered, setRemembered] = useState<boolean | null>(null);
  const [message, setMessage] = useState('');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  const [attempt, setAttempt] = useState(0);
  const requireSignIn = useCallback(() => {
    if (!remote) return;
    setPhase('required');
    setMessage('Sign in again to continue.');
  }, [remote]);

  useEffect(() => {
    if (!remote) return;
    const controller = new AbortController();
    setPhase('checking');
    getStatus(controller.signal).then(() => {
      if (!controller.signal.aborted) setPhase('ready');
    }).catch(reason => {
      if (!controller.signal.aborted) setPhase(isAuthenticationError(reason) ? 'required' : 'offline');
    });
    return () => controller.abort();
  }, [remote, attempt]);

  const logout = async () => {
    await logoutDashboard();
    setRemembered(null);
    setToken('');
    setError('');
    setMessage('You are logged out. This browser is no longer remembered.');
    setPhase('required');
  };
  const signIn = async (event: React.FormEvent) => {
    event.preventDefault();
    setBusy(true);
    setError('');
    try {
      await authenticateDashboard(token, remember);
      setRemembered(remember);
      setMessage('');
      setPhase('checking');
      setAttempt(value => value + 1);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Sign in failed. Try again.');
    } finally {
      setToken('');
      setBusy(false);
    }
  };
  if (phase === 'ready') {
    return <AccessContext.Provider value={{ remote, remembered, requireSignIn, logout }}>{children}</AccessContext.Provider>;
  }
  return (
    <div className="dashboard-access flex min-h-dvh items-center justify-center bg-void-black px-3 py-10 text-text-primary">
      <main className="w-full max-w-[360px]">
        <div className="flex items-center gap-2.5">
          <img src={logoUrl} alt="" className="h-7 w-7 rounded object-cover" />
          <span className="text-sm font-semibold">InferDeck</span>
        </div>
        {phase === 'checking' ? (
          <div className="mt-8">
            <h1 className="text-xl font-semibold">Checking your session</h1>
            <p role="status" className="mt-1 flex items-center gap-2 text-sm text-text-muted">
              <span className="h-3 w-3 animate-spin rounded-full border-2 border-queue-blue border-t-transparent" aria-hidden="true" />
              Connecting to InferDeck.
            </p>
          </div>
        ) : phase === 'offline' ? (
          <div className="mt-8">
            <h1 className="text-xl font-semibold">Gateway unavailable</h1>
            <p className="mt-1 text-sm text-text-muted">Check that InferDeck is running and this device can reach it.</p>
            <p className="mt-1 text-sm text-text-muted">Your login has not been cleared.</p>
            <button className="mt-6 min-h-10 w-full rounded-md bg-queue-blue px-4 text-sm font-semibold text-on-accent hover:bg-queue-blue/90" onClick={() => setAttempt(value => value + 1)}>Retry connection</button>
          </div>
        ) : (
          <form className="mt-8" onSubmit={event => { void signIn(event); }}>
            <h1 className="text-xl font-semibold">Sign in</h1>
            <p className="mt-1 text-sm text-text-muted">Enter your dashboard key to continue.</p>
            {message && <p role="status" className="mt-4 rounded-md border border-border-slate bg-panel-slate px-3 py-2 text-sm text-text-secondary">{message}</p>}
            <label className="mt-6 block text-sm text-text-secondary" htmlFor="dashboard-token">Dashboard key</label>
            <input id="dashboard-token" className="mt-1.5 h-10 w-full px-3 text-base" type="password" autoComplete="current-password" required value={token} onChange={event => setToken(event.target.value)} aria-describedby="dashboard-key-help" aria-invalid={!!error} disabled={busy} />
            <p id="dashboard-key-help" className="mt-1.5 text-xs text-text-muted">Use your dashboard key, not an inference API key.</p>
            <label className="mt-4 flex items-center gap-2 text-sm text-text-secondary">
              <input type="checkbox" className="h-4 w-4" checked={remember} onChange={event => setRemember(event.target.checked)} disabled={busy} />
              Remember this browser
            </label>
            <button className="mt-6 min-h-10 w-full rounded-md bg-queue-blue px-4 text-sm font-semibold text-on-accent hover:bg-queue-blue/90 disabled:opacity-40" type="submit" disabled={busy || !token}>{busy ? 'Signing in...' : 'Sign in'}</button>
            {error && <p className="mt-3 text-sm text-danger-rose" role="alert">{error}</p>}
            <p className="mt-4 text-xs text-text-muted">On a shared device, leave this unchecked. Log out to remove access from this browser.</p>
          </form>
        )}
      </main>
    </div>
  );
};
