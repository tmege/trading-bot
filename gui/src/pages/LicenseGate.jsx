import React, { useState, useEffect } from 'react';
import { Shield, Copy, Check, AlertCircle, Loader2 } from 'lucide-react';

export default function LicenseGate({ onActivated }) {
  const [machineId, setMachineId] = useState('');
  const [token, setToken] = useState('');
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);
  const [copied, setCopied] = useState(false);

  useEffect(() => {
    window.api.license.machineId().then(res => {
      if (res.ok) setMachineId(res.machineId);
    });
  }, []);

  async function handleActivate(e) {
    e.preventDefault();
    const trimmed = token.trim();
    if (!trimmed) return;

    setError('');
    setLoading(true);

    try {
      const res = await window.api.license.activate(trimmed);
      if (res.ok) {
        onActivated();
      } else {
        setError(res.error || 'Activation echouee');
      }
    } catch (err) {
      setError(err.message || 'Erreur inattendue');
    } finally {
      setLoading(false);
    }
  }

  function handleCopy() {
    navigator.clipboard.writeText(machineId).then(() => {
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    });
  }

  return (
    <div className="flex flex-col h-screen w-screen bg-surface-bg">
      {/* Draggable titlebar */}
      <div className="titlebar h-9 shrink-0 flex items-center border-b border-surface-border bg-surface-card">
        <div className="w-20 shrink-0" />
        <span className="text-xs text-gray-500 font-medium tracking-wider select-none">
          TRADING BOT
        </span>
      </div>

      <div className="flex-1 flex items-center justify-center p-6">
        <div className="w-full max-w-md">
          {/* Header */}
          <div className="flex flex-col items-center mb-8">
            <div className="w-14 h-14 rounded-2xl bg-blue-500/10 flex items-center justify-center mb-4">
              <Shield className="w-7 h-7 text-blue-400" />
            </div>
            <h1 className="text-xl font-semibold text-gray-100">Activation requise</h1>
            <p className="text-sm text-gray-500 mt-2 text-center">
              Entrez votre token de licence pour activer l'application.
            </p>
          </div>

          {/* Card */}
          <div className="bg-surface-card border border-surface-border rounded-xl p-6 space-y-5">
            {/* Machine ID */}
            <div>
              <label className="block text-xs font-medium text-gray-400 mb-2">
                Identifiant machine
              </label>
              <div className="flex items-center gap-2">
                <code className="flex-1 bg-surface-bg border border-surface-border rounded-lg px-3 py-2 text-sm text-gray-300 font-mono select-all">
                  {machineId || '...'}
                </code>
                <button
                  onClick={handleCopy}
                  disabled={!machineId}
                  className="shrink-0 p-2 rounded-lg border border-surface-border hover:bg-surface-bg text-gray-400 hover:text-gray-200 transition-colors disabled:opacity-40"
                  title="Copier"
                >
                  {copied ? <Check className="w-4 h-4 text-green-400" /> : <Copy className="w-4 h-4" />}
                </button>
              </div>
              <p className="text-xs text-gray-600 mt-1.5">
                Communiquez cet identifiant a l'administrateur pour obtenir votre token.
              </p>
            </div>

            {/* Token input */}
            <form onSubmit={handleActivate}>
              <label className="block text-xs font-medium text-gray-400 mb-2">
                Token d'activation
              </label>
              <textarea
                value={token}
                onChange={e => { setToken(e.target.value); setError(''); }}
                placeholder="Collez votre token ici..."
                rows={3}
                className="w-full bg-surface-bg border border-surface-border rounded-lg px-3 py-2 text-sm text-gray-200 font-mono placeholder-gray-600 resize-none focus:outline-none focus:ring-1 focus:ring-blue-500/50 focus:border-blue-500/50"
                autoFocus
              />

              {/* Error */}
              {error && (
                <div className="flex items-start gap-2 mt-3 p-3 rounded-lg bg-red-500/10 border border-red-500/20">
                  <AlertCircle className="w-4 h-4 text-red-400 shrink-0 mt-0.5" />
                  <span className="text-sm text-red-300">{error}</span>
                </div>
              )}

              {/* Submit */}
              <button
                type="submit"
                disabled={loading || !token.trim()}
                className="w-full mt-4 py-2.5 rounded-lg bg-blue-600 hover:bg-blue-500 text-white text-sm font-medium transition-colors disabled:opacity-40 disabled:cursor-not-allowed flex items-center justify-center gap-2"
              >
                {loading ? (
                  <>
                    <Loader2 className="w-4 h-4 animate-spin" />
                    Verification...
                  </>
                ) : (
                  'Activer la licence'
                )}
              </button>
            </form>
          </div>
        </div>
      </div>
    </div>
  );
}
