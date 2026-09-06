import React, { createContext, useCallback, useContext, useEffect, useState } from 'react';
import { authenticateDashboard, getStatus, isAuthenticationError, logoutDashboard } from '../api';

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
    <div className="dashboard-access min-h-screen bg-black text-white">
      <header className="flex items-center justify-between border-b border-white/20 px-6 py-5">
        <strong>InferDeck</strong><span className="text-sm">Dashboard access</span>
      </header>
      <main className="mx-auto w-full max-w-md px-6 py-16">
        {phase === 'checking' ? <><h1 className="text-2xl font-semibold">Checking your session</h1><p role="status" className="mt-3">Connecting to InferDeck.</p></> :
          phase === 'offline' ? <><h1 className="text-2xl font-semibold">Gateway unavailable</h1><p className="mt-3">Check that InferDeck is running and this device can reach it.</p><p className="mt-3">Your login has not been cleared.</p><button className="mt-6 min-h-11 border border-white px-4" onClick={() => setAttempt(value => value + 1)}>Retry connection</button></> :
          <form onSubmit={event => { void signIn(event); }}>
            <h1 className="text-2xl font-semibold">Sign in</h1>
            <p className="mt-3">Access your InferDeck dashboard.</p>
            {message && <p role="status" className="mt-4">{message}</p>}
            <label className="mt-8 block" htmlFor="dashboard-token">Dashboard key</label>
            <input id="dashboard-token" className="mt-2 min-h-11 w-full px-3" type="password" autoComplete="current-password" required value={token} onChange={event => setToken(event.target.value)} aria-describedby="dashboard-key-help" aria-invalid={!!error} disabled={busy} />
            <p id="dashboard-key-help" className="mt-2 text-sm">Use your dashboard key, not an inference API key.</p>
            <label className="mt-5 flex min-h-11 items-center gap-3"><input type="checkbox" checked={remember} onChange={event => setRemember(event.target.checked)} disabled={busy} />Remember this browser</label>
            <button className="mt-3 min-h-11 w-full bg-white px-4 font-semibold text-black disabled:opacity-50" type="submit" disabled={busy || !token}>{busy ? 'Signing in...' : 'Sign in'}</button>
            {error && <p className="mt-4 text-danger-rose" role="alert">{error}</p>}
            <p className="mt-4 text-sm">On a shared device, leave this unchecked. Log out to remove access from this browser.</p>
          </form>}
      </main>
    </div>
  );
};
